qt_add_executable(cutpilot-app
    main.cpp)

set_target_properties(cutpilot-app PROPERTIES
    MACOSX_BUNDLE TRUE
    OUTPUT_NAME "CutPilot")

qt_add_resources(cutpilot-app "cutpilot_app_qml"
    PREFIX "/cutpilot/app"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        Canvas.qml)

target_link_libraries(cutpilot-app PRIVATE
    cutpilot-render
    cutpilot-theme
    Qt6::Widgets
    Qt6::QuickWidgets
    Qt6::Quick)
