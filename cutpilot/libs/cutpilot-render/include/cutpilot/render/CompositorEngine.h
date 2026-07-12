#pragma once

#include "cutpilot/core/CompositePlan.h"

#include <QImage>

#include <memory>

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiCommandBuffer;
class QRhiTexture;
QT_END_NAMESPACE

namespace cutpilot::render {

// Evaluates a composite plan as GPU render passes over QRhi: one full-surface
// pass per compositing node into its own texture, dependency order, straight
// alpha. Source pixels (stills, generation results) are uploaded once per
// version; every pass's output is cached under its plan signature plus the
// versions of the sources feeding the plan, so re-evaluating an unchanged
// graph records nothing and a parameter scrub re-renders only the passes
// whose subtree actually changed.
//
// This class and the preview renderer are, with the grid renderer, the QRhi
// boundary: no QRhi type leaks past this header, and the implementation is
// reviewed on every Qt upgrade alongside the version pin.
class CompositorEngine {
public:
    CompositorEngine();
    ~CompositorEngine();

    CompositorEngine(const CompositorEngine &) = delete;
    CompositorEngine &operator=(const CompositorEngine &) = delete;

    // The device everything lives on; not owned. Changing it drops every
    // texture, pipeline, and cached pass.
    void setRhi(QRhi *rhi);
    QRhi *rhi() const;

    // Create and own a windowless device instead — the standalone mode tests
    // and thumbnail rendering run on. False when the platform's GPU backend
    // cannot initialize.
    bool adoptHeadlessDevice();

    // Hand the engine a source node's pixels. Re-uploading the same version
    // is a no-op; a new version invalidates the passes it feeds. Oversized
    // images are clamped to a bounded texture dimension.
    void setSource(int nodeId, const QImage &image, int version);
    int sourceVersion(int nodeId) const;

    // Drop everything the engine holds for a node that left the graph.
    void releaseNode(int nodeId);

    // Record the stale passes of the plan onto the command buffer (outside
    // any render pass) and return the target's texture — cached or fresh.
    // Null when the plan is invalid or the device is missing.
    QRhiTexture *evaluate(const core::CompositePlan &plan, QRhiCommandBuffer *cb);

    // Standalone evaluation on this engine's own frame, read back to an
    // image. For tests and thumbnail rendering; never call it mid-frame.
    QImage evaluateToImage(const core::CompositePlan &plan);

    // How many passes the last evaluate actually recorded — the observable
    // proof that unchanged subtrees are served from cache.
    int lastPassCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace cutpilot::render
