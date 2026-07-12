"""The asynchronous job queue.

A submitted job runs on its own worker thread and publishes versioned state
snapshots; subscribers block on the job's condition variable until a newer
version lands, which is what the server's event stream loops on. Cancellation
is cooperative: the flag is checked between progress steps.
"""

from __future__ import annotations

import hashlib
import os
import sys
import threading
import traceback
import uuid
from dataclasses import asdict, dataclass

from .providers import GenerationRequest, JobCanceled, MissingKeyError, provider_for
from .registry import ModelInfo

TERMINAL_STATES = ("done", "error", "canceled")


@dataclass
class JobSnapshot:
    job_id: str
    state: str
    progress: float
    message: str
    result_path: str
    # SHA-256 of the finished result file: the result's identity, so a
    # consumer can key derived work off the actual bytes it received.
    result_digest: str
    cost_usd: float
    width: int
    height: int
    version: int

    def to_dict(self) -> dict:
        return asdict(self)


class Job:
    def __init__(self, job_id: str):
        self._condition = threading.Condition()
        self._snapshot = JobSnapshot(
            job_id=job_id,
            state="queued",
            progress=0.0,
            message="",
            result_path="",
            result_digest="",
            cost_usd=-1.0,
            width=0,
            height=0,
            version=1,
        )
        self.cancel_requested = False

    @property
    def id(self) -> str:
        return self._snapshot.job_id

    def snapshot(self) -> JobSnapshot:
        with self._condition:
            return JobSnapshot(**asdict(self._snapshot))

    def update(self, **fields) -> None:
        with self._condition:
            for name, value in fields.items():
                setattr(self._snapshot, name, value)
            self._snapshot.version += 1
            self._condition.notify_all()

    def wait_newer(self, version: int, timeout: float) -> JobSnapshot:
        """The first snapshot newer than version, or the current one on timeout."""
        with self._condition:
            self._condition.wait_for(
                lambda: self._snapshot.version > version, timeout=timeout
            )
            return JobSnapshot(**asdict(self._snapshot))


class JobManager:
    def __init__(self, gen_dir: str):
        self._gen_dir = gen_dir
        self._jobs: dict[str, Job] = {}
        self._lock = threading.Lock()
        os.makedirs(gen_dir, exist_ok=True)

    def get(self, job_id: str) -> Job | None:
        with self._lock:
            return self._jobs.get(job_id)

    def submit(
        self,
        model: ModelInfo,
        prompt: str,
        width: int,
        height: int,
        seed: int,
        input_path: str = "",
    ) -> Job:
        job = Job(uuid.uuid4().hex[:12])
        with self._lock:
            self._jobs[job.id] = job

        request = GenerationRequest(
            model=model,
            prompt=prompt,
            width=width,
            height=height,
            seed=seed,
            out_path=os.path.join(self._gen_dir, f"{job.id}.png"),
            input_path=input_path,
        )
        worker = threading.Thread(
            target=self._run, args=(job, request), name=f"gen-{job.id}", daemon=True
        )
        worker.start()
        return job

    def cancel(self, job_id: str) -> bool:
        job = self.get(job_id)
        if job is None:
            return False
        job.cancel_requested = True
        return True

    def _run(self, job: Job, request: GenerationRequest) -> None:
        if job.cancel_requested:
            job.update(state="canceled", message="Stopped")
            return
        job.update(state="running", progress=0.0)
        provider = provider_for(request.model)
        try:
            result = provider.generate(
                request,
                on_progress=lambda fraction: job.update(
                    progress=min(0.99, max(0.0, fraction))
                ),
                is_canceled=lambda: job.cancel_requested,
            )
        except JobCanceled:
            job.update(state="canceled", message="Stopped")
            return
        except MissingKeyError as exc:
            job.update(state="error", message=str(exc))
            return
        except Exception as exc:  # surface any vendor failure as a job error
            traceback.print_exc(file=sys.stderr)
            job.update(state="error", message=str(exc))
            return

        if job.cancel_requested:
            job.update(state="canceled", message="Stopped")
            return

        digest = hashlib.sha256()
        try:
            with open(result.path, "rb") as handle:
                for block in iter(lambda: handle.read(1 << 16), b""):
                    digest.update(block)
        except OSError as exc:
            job.update(state="error", message=f"Result file unreadable: {exc}")
            return

        job.update(
            state="done",
            progress=1.0,
            result_path=result.path,
            result_digest=digest.hexdigest(),
            cost_usd=result.cost_usd,
            width=result.width,
            height=result.height,
        )
