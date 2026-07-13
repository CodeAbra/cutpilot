# Headless domain-core tests. Each target links Qt6::Test and cutpilot-core only,
# so the tested logic stays free of any GUI dependency. Sibling executables are
# added a line at a time as new core surfaces land. The one exception at the
# bottom drives the canvas layer's wiring gestures with synthesized events on the
# offscreen platform — still windowless, but linked against the render library.

qt_add_executable(tst_theme tst_theme.cpp)
target_link_libraries(tst_theme PRIVATE Qt6::Test cutpilot-theme)
add_test(NAME theme.tokens COMMAND tst_theme)

# The pure camera/minimap mapping math: header-only value types from the render
# library, exercised without any GUI.
qt_add_executable(tst_minimap tst_minimap.cpp)
target_link_libraries(tst_minimap PRIVATE Qt6::Test cutpilot-render)
add_test(NAME render.minimap COMMAND tst_minimap)

qt_add_executable(tst_nodegraph tst_nodegraph.cpp)
target_link_libraries(tst_nodegraph PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.nodegraph COMMAND tst_nodegraph)

qt_add_executable(tst_spatialindex tst_spatialindex.cpp)
target_link_libraries(tst_spatialindex PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.spatialindex COMMAND tst_spatialindex)

qt_add_executable(tst_commandstack tst_commandstack.cpp)
target_link_libraries(tst_commandstack PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.commandstack COMMAND tst_commandstack)

qt_add_executable(tst_alignmentguides tst_alignmentguides.cpp)
target_link_libraries(tst_alignmentguides PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.alignmentguides COMMAND tst_alignmentguides)

qt_add_executable(tst_connections tst_connections.cpp)
target_link_libraries(tst_connections PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.connections COMMAND tst_connections)

qt_add_executable(tst_connectorpath tst_connectorpath.cpp)
target_link_libraries(tst_connectorpath PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.connectorpath COMMAND tst_connectorpath)

qt_add_executable(tst_generationcommands tst_generationcommands.cpp)
target_link_libraries(tst_generationcommands PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.generationcommands COMMAND tst_generationcommands)

qt_add_executable(tst_pipelineorder tst_pipelineorder.cpp)
target_link_libraries(tst_pipelineorder PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.pipelineorder COMMAND tst_pipelineorder)

qt_add_executable(tst_composite tst_composite.cpp)
target_link_libraries(tst_composite PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.composite COMMAND tst_composite)

qt_add_executable(tst_comfyimport tst_comfyimport.cpp)
target_link_libraries(tst_comfyimport PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.comfyimport COMMAND tst_comfyimport)

qt_add_executable(tst_workflowjson tst_workflowjson.cpp)
target_link_libraries(tst_workflowjson PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.workflowjson COMMAND tst_workflowjson)

qt_add_executable(tst_timeline tst_timeline.cpp)
target_link_libraries(tst_timeline PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.timeline COMMAND tst_timeline)

qt_add_executable(tst_edl tst_edl.cpp)
target_link_libraries(tst_edl PRIVATE Qt6::Test cutpilot-core)
add_test(NAME core.edl COMMAND tst_edl)

qt_add_executable(tst_nodelayerwiring tst_nodelayerwiring.cpp)
target_link_libraries(tst_nodelayerwiring PRIVATE Qt6::Test cutpilot-render)
add_test(NAME render.nodelayerwiring COMMAND tst_nodelayerwiring)

# The canvas tools and the minimap over the same synthesized-event harness:
# slicing, armed placement, body-to-body wiring, frame carries, palette entry
# points, the undo seams, and minimap mapping/navigation against live state.
qt_add_executable(tst_canvastools tst_canvastools.cpp)
target_link_libraries(tst_canvastools PRIVATE Qt6::Test cutpilot-render)
add_test(NAME render.canvastools COMMAND tst_canvastools)

# The GPU compositor on a real windowless device: per-op pixel math against
# reference formulas, matte conventions, and cache-driven pass counts. Skips
# only when the platform has no headless GPU backend.
qt_add_executable(tst_compositor tst_compositor.cpp)
target_link_libraries(tst_compositor PRIVATE Qt6::Test cutpilot-render)
add_test(NAME render.compositor COMMAND tst_compositor)

# The thumbnail service over a live compositing chain: card pixels arrive
# through the event loop without blocking the GUI thread, and follow source
# changes. Skips without a headless GPU device.
qt_add_executable(tst_compositorservice tst_compositorservice.cpp)
target_link_libraries(tst_compositorservice PRIVATE Qt6::Test cutpilot-render)
add_test(NAME render.compositorservice COMMAND tst_compositorservice)

qt_add_executable(tst_previewwiring tst_previewwiring.cpp)
target_link_libraries(tst_previewwiring PRIVATE Qt6::Test cutpilot-render)
add_test(NAME render.previewwiring COMMAND tst_previewwiring)

# The video source over a generated two-color asset: proxy frame delivery,
# non-blocking scrubs, self-advancing playback. Skips without ffmpeg.
qt_add_executable(tst_videosource tst_videosource.cpp)
target_link_libraries(tst_videosource PRIVATE Qt6::Test cutpilot-render)
add_test(NAME render.videosource COMMAND tst_videosource)

# The full generation flow: a live sidecar process, the coordinator, and a
# prompt-feeds-generate graph, driven end to end on the offscreen platform.
qt_add_executable(tst_generationflow tst_generationflow.cpp)
target_link_libraries(tst_generationflow PRIVATE Qt6::Test cutpilot-ipc cutpilot-render)
add_test(NAME ipc.generationflow COMMAND tst_generationflow)

# The pipeline over a live sidecar: evaluation order, dirty tracking and
# cache hits asserted by submission counts, cost gates and the run cap,
# forced re-runs, and mid-run edits.
qt_add_executable(tst_pipelineflow tst_pipelineflow.cpp)
target_link_libraries(tst_pipelineflow PRIVATE Qt6::Test cutpilot-ipc)
add_test(NAME ipc.pipelineflow COMMAND tst_pipelineflow)

# The export path over a live convert sidecar: the bundle on disk, FCPXML
# and OTIO re-parsed structurally, the ComfyUI tier report, and the Resolve
# bridge's refusals.
qt_add_executable(tst_convertflow tst_convertflow.cpp)
target_link_libraries(tst_convertflow PRIVATE Qt6::Test cutpilot-ipc)
add_test(NAME ipc.convertflow COMMAND tst_convertflow)

# The generation sidecar's own suite, run against a live loopback server.
# Keychain lookups are disabled so key-presence checks are driven by the
# test environment alone.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME sidecar.generation
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -t .
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/sidecars/generation)
    set_tests_properties(sidecar.generation PROPERTIES
        ENVIRONMENT "CUTPILOT_DISABLE_KEYCHAIN=1")

    # The convert sidecar's suite: interchange writers validated by
    # re-parsing against each format's grammar, the ComfyUI mapper's tier
    # grading, and the Resolve bridge's refusal paths.
    add_test(NAME sidecar.convert
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -t .
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/sidecars/convert)
endif()

# The export controller over a live convert sidecar: a headless bundle
# export, results landing in the project store as Generator segments, and
# the one-import-at-a-time guard on the shared convert client.
qt_add_executable(tst_exportcontroller tst_exportcontroller.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/ExportController.cpp)
target_include_directories(tst_exportcontroller PRIVATE
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app)
target_link_libraries(tst_exportcontroller PRIVATE Qt6::Test Qt6::Widgets
    cutpilot-ipc cutpilot-render)
add_test(NAME app.exportcontroller COMMAND tst_exportcontroller)

# The workflow's autosave home: save/load round trip, debounced bursts,
# name fallback and persistence, and failure reporting.
qt_add_executable(tst_workflowstore tst_workflowstore.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/WorkflowStore.cpp)
target_include_directories(tst_workflowstore PRIVATE
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app)
target_link_libraries(tst_workflowstore PRIVATE Qt6::Test cutpilot-core)
add_test(NAME app.workflowstore COMMAND tst_workflowstore)

# The command palette's data and keyboard flow: taxonomy sections, the
# incremental filter across titles/categories/models, offer mode, and
# choose/dismiss driven entirely by keys.
qt_add_executable(tst_commandpalette tst_commandpalette.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/CommandPalette.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/PaletteModel.cpp)
target_include_directories(tst_commandpalette PRIVATE
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app)
target_link_libraries(tst_commandpalette PRIVATE Qt6::Test Qt6::Widgets
    cutpilot-core cutpilot-theme)
add_test(NAME app.commandpalette COMMAND tst_commandpalette)

# Reusable templates and the theme owner: selection capture strips run
# artifacts while keeping media sources, the template document round-trips,
# and the theme choice cycles and persists.
qt_add_executable(tst_workflowtemplates tst_workflowtemplates.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/ThemeController.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/WorkflowTemplates.cpp)
target_include_directories(tst_workflowtemplates PRIVATE
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app)
target_link_libraries(tst_workflowtemplates PRIVATE Qt6::Test cutpilot-core
    cutpilot-theme)
add_test(NAME app.workflowtemplates COMMAND tst_workflowtemplates)

# The pill, cluster, and top bar against a real layer and camera: tool
# state round trips, history enablement, zoom/fit, minimap toggles, mode
# tabs, and the inline name commit.
qt_add_executable(tst_chromewiring tst_chromewiring.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/CanvasCluster.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/ToolPill.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/TopBar.cpp)
target_include_directories(tst_chromewiring PRIVATE
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app)
target_link_libraries(tst_chromewiring PRIVATE Qt6::Test Qt6::Widgets
    cutpilot-render)
add_test(NAME app.chromewiring COMMAND tst_chromewiring)

# The composite inspector against the real command stack: a scrub commits one
# undo step, and an undo taken while the panel stays open must survive the
# panel's next gesture untouched.
qt_add_executable(tst_compositeinspector tst_compositeinspector.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/CompositeInspector.cpp)
target_include_directories(tst_compositeinspector PRIVATE
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app)
target_link_libraries(tst_compositeinspector PRIVATE Qt6::Test Qt6::Widgets
    cutpilot-render)
add_test(NAME app.compositeinspector COMMAND tst_compositeinspector)

# The quick surface over the real stack: a live sidecar, the real coordinator
# and cache, a real layer — one prompt materializes a real generate node,
# runs it to a sized result, honors the cache, surfaces the add-a-key path,
# and leaves everything on the canvas when dismissed.
qt_add_executable(tst_quickmode tst_quickmode.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/QuickPanel.cpp)
target_include_directories(tst_quickmode PRIVATE
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app)
target_link_libraries(tst_quickmode PRIVATE Qt6::Test Qt6::Widgets
    cutpilot-render cutpilot-ipc)
add_test(NAME app.quickmode COMMAND tst_quickmode)

# The Node screen as one working whole: a palette pick lands on the live
# canvas, generates through a real sidecar, shows on the minimap mapping,
# undoes and redoes through the cluster's controls under one identity, and
# is reachable from the rail's Content and Search panels.
qt_add_executable(tst_nodescreen tst_nodescreen.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/CanvasCluster.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/CommandPalette.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/PaletteModel.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/RailPanels.cpp
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app/WorkflowTemplates.cpp)
target_include_directories(tst_nodescreen PRIVATE
    ${CMAKE_SOURCE_DIR}/app/cutpilot-app)
target_link_libraries(tst_nodescreen PRIVATE Qt6::Test Qt6::Widgets
    cutpilot-render cutpilot-ipc)
add_test(NAME app.nodescreen COMMAND tst_nodescreen)
