qt_add_executable(cutpilot-app
    CompositeInspector.cpp
    CompositeInspector.h
    ExportController.cpp
    ExportController.h
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
