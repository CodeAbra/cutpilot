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
- A reference image (Still Image node) wired into an image-consuming
  generation now genuinely feeds the run: the picked file travels into the
  job and its content keys the result cache, so swapping the file
  re-generates while an unchanged reference is served from cache. Previously
  only a finished generation's result could feed an image input, and an
  image-consuming model fed by a reference alone refused to run.

### Changed
- Quick Mode adoption resolves by the recorded node identity instead of the
  node title. Renaming the quick node no longer detaches the surface, and a
  duplicate node titled "Quick Generate" (for example from a placed template)
  can no longer be adopted in its place. Documents saved before the binding
  existed adopt their quick node by the old title rule once, on first load.
