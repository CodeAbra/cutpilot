#pragma once

#include <QString>

namespace cutpilot::ipc {

// One entry of the generation service's model registry. hasKey reports whether
// the vendor's BYOK key is currently configured — never the key itself.
struct ModelInfo {
    QString id;
    QString label;
    QString provider;
    double priceUsd = 0.0;
    bool needsKey = false;
    bool hasKey = false;
};

// A job's lifecycle as the service streams it.
enum class JobState {
    Queued,
    Running,
    Done,
    Error,
    Canceled
};

// One streamed snapshot of a job.
struct JobUpdate {
    QString jobId;
    JobState state = JobState::Queued;
    double progress = 0.0;
    QString message;
    QString resultPath;
    double costUsd = -1.0;
    int width = 0;
    int height = 0;
};

// Why a submission was turned down before a job existed.
enum class SubmitRefusal {
    MissingKey,
    Invalid,
    Unavailable
};

// What the canvas asks a generation model for.
struct GenerationRequest {
    QString modelId;
    QString prompt;
    int width = 768;
    int height = 512;
    int seed = 0;
};

} // namespace cutpilot::ipc
