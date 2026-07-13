# Changelog

All notable changes to CutPilot are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Every node in the workflow document now carries a durable identity (`uid`),
  persisted in the workflow JSON and preserved across save/reload, undo/redo,
  and renames. Documents written before this field existed gain identities on
  load, and those identities then persist.
- The workflow document records which node the Quick Mode surface owns
  (`quickNode`), so reopening a project re-adopts the same node.

### Fixed
- The one-time title-based Quick Mode adoption now applies only to documents
  genuinely written before node identities existed (no `uid` stored on any
  node). A current document with no recorded binding stays unbound: a node
  coincidentally titled "Quick Generate" — for example a placed template
  carrying a saved copy — is no longer silently adopted as the Quick Mode
  node on load. A recorded binding is also validated to name a generate
  node; a binding of any other kind is dropped on load and refused by the
  Quick Mode surface.
- Redoing an undone node placement (or ComfyUI import) restores the node
  exactly as the undo removed it. Previously a generation result or picked
  file that arrived after the placement was dropped from the document by the
  undo/redo round trip.
- The same guarantee now holds for deletion: undoing a redone delete restores
  the node as the redo removed it, so a generation result that landed between
  the undo and the redo survives the history walk instead of reverting to the
  snapshot captured at first delete.
- Undo/redo no longer replays node selection: a node restored by the history
  walk comes back with the selection it had when the step was first recorded,
  not whatever the selection happened to be at undo time.
- A reference image (Still Image node) wired into an image-consuming
  generation now genuinely feeds the run: the picked file travels into the
  job and its content keys the result cache, so swapping the file
  re-generates while an unchanged reference is served from cache. Previously
  only a finished generation's result could feed an image input, and an
  image-consuming model fed by a reference alone refused to run.
- A reference file rewritten with the same byte size and modification time
  (a timestamp-preserving tool, or two writes inside one clock tick) no
  longer serves the stale cached result: change detection also watches the
  file's metadata-change time, and a freshly written file is re-hashed until
  its clocks can be trusted.
- Running an image-consuming generation whose wired reference file was moved
  or deleted outside the app now refuses locally with "Reference file
  missing" instead of submitting a job that names an unreadable file.
- Starting a run no longer stalls the interface while a large reference or
  result file is content-hashed: files past a small inline budget hash on a
  worker thread, the run holds that node until its digest arrives, and cache
  reuse behaves exactly as before. A cached digest is also no longer trusted
  when it was computed inside the freshly-written window of a file whose
  clocks later settled — the next run re-hashes once and trusts from there.
- Compositing-node thumbnails render on their own GPU thread and land on the
  cards through the event loop, so a board full of heavy composites no longer
  freezes the interface on every edit's refresh tick. Building the refresh
  pass itself also stopped recomputing every upstream signature once per
  downstream node, which alone stalled deep chains for noticeable fractions
  of a second.
- Running a generation whose image input is wired from a compositing node now
  says "Composite outputs can't feed generations" instead of the misleading
  "Connect an image input" — the wire is legal under the port rules, but a
  composite renders a texture with no file behind it, so nothing can reach
  the job yet.

### Changed
- Quick Mode adoption resolves by the recorded node identity instead of the
  node title. Renaming the quick node no longer detaches the surface, and a
  duplicate node titled "Quick Generate" (for example from a placed template)
  can no longer be adopted in its place. Documents saved before the binding
  existed adopt their quick node by the old title rule once, on first load.
