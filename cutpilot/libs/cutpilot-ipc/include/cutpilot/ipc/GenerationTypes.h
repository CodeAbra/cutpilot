#pragma once

#include <QString>
#include <QVector>

namespace cutpilot::ipc {

// One secret a provider needs, as the key surface renders it: the label beside
// its masked field and the keychain account it is written to. The account is
// supplied by the service, never constructed on the client, so the surface and
// the service always agree on where a secret lives. Never carries a value.
struct SecretSlot {
    QString name;
    QString label;
    QString account;
};

// One entry of the generation service's model registry. hasKey reports whether
// the vendor's BYOK key is currently configured — never the key itself.
// needsPrompt and needsInput declare what the model consumes, so a run can be
// gated before it is ever submitted.
struct ModelInfo {
    QString id;
    QString label;
    QString provider;
    double priceUsd = 0.0;
    bool needsKey = false;
    bool hasKey = false;
    bool needsPrompt = true;
    bool needsInput = false;
    // Kept out of the model picker: a model whose vendor contract is not yet
    // confirmed against a live key, or a keyless local driver. Still resolvable
    // by explicit id so a node that already carries it can run.
    bool unverified = false;
    // The secrets this vendor needs, on the key-vendor channel only. Empty on a
    // picker/runnable row; the key surface reads it to render one masked field
    // per slot. Never carries a value.
    QVector<SecretSlot> secretSlots;
};

// A job's lifecycle as the service streams it.
enum class JobState {
    Queued,
    Running,
    Done,
    Error,
    Canceled
};

// One streamed snapshot of a job. resultDigest is the SHA-256 of the finished
// result file — the result's identity for downstream cache keys.
struct JobUpdate {
    QString jobId;
    JobState state = JobState::Queued;
    double progress = 0.0;
    QString message;
    QString resultPath;
    QString resultDigest;
    double costUsd = -1.0;
    int width = 0;
    int height = 0;
    // The result file's kind; a frame absent the field stays an image.
    QString resultKind = QStringLiteral("image");
};

// Why a submission was turned down before a job existed.
enum class SubmitRefusal {
    MissingKey,
    Invalid,
    Unavailable
};

// What the canvas asks a generation model for. inputPath carries the upstream
// result an image-consuming model derives from.
struct GenerationRequest {
    QString modelId;
    QString prompt;
    QString inputPath;
    int width = 768;
    int height = 512;
    int seed = 0;
};

// A live snapshot of the pipeline run, for the run summary surface. spent is
// the actual cost of finished jobs this run; committed is the estimates of
// jobs still in flight; a cap of zero means no cap.
struct RunSummary {
    bool active = false;
    bool paused = false;
    QString pauseReason;
    int total = 0;
    int fresh = 0;
    int reused = 0;
    int running = 0;
    int held = 0;
    int failed = 0;
    double spentUsd = 0.0;
    double committedUsd = 0.0;
    double capUsd = 0.0;

    int settled() const { return fresh + reused + failed; }
    int percent() const { return total > 0 ? settled() * 100 / total : 0; }
};

} // namespace cutpilot::ipc
