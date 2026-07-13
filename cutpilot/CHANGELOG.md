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

### Changed
- Quick Mode adoption resolves by the recorded node identity instead of the
  node title. Renaming the quick node no longer detaches the surface, and a
  duplicate node titled "Quick Generate" (for example from a placed template)
  can no longer be adopted in its place. Documents saved before the binding
  existed adopt their quick node by the old title rule once, on first load.
