qt_add_executable(cutpilot-app
    CanvasCluster.cpp
    CanvasCluster.h
    CommandPalette.cpp
    CommandPalette.h
    CompositeInspector.cpp
    CompositeInspector.h
    ExportController.cpp
    ExportController.h
    KeyManagerPanel.cpp
    KeyManagerPanel.h
    PaletteModel.cpp
    PaletteModel.h
    QuickPanel.cpp
    QuickPanel.h
    RailPanels.cpp
    RailPanels.h
    ThemeController.cpp
    ThemeController.h
    ToolPill.cpp
    ToolPill.h
    ToolRail.cpp
    ToolRail.h
    TopBar.cpp
    TopBar.h
    WorkflowStore.cpp
    WorkflowStore.h
    WorkflowTemplates.cpp
    WorkflowTemplates.h
    main.cpp)

set_target_properties(cutpilot-app PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "CutPilot")

qt_add_resources(cutpilot-app "cutpilot_app_qml"
    PREFIX "/cutpilot/app"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        Canvas.qml
        Preview.qml)

target_link_libraries(cutpilot-app PRIVATE
    cutpilot-render
    cutpilot-theme
    cutpilot-ipc
    otf-secrets
    Qt6::Widgets
    Qt6::QuickWidgets
    Qt6::Quick)
