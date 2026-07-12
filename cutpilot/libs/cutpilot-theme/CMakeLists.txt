add_library(cutpilot-theme STATIC
    src/ThemeTable.cpp)

target_include_directories(cutpilot-theme PUBLIC include)

target_link_libraries(cutpilot-theme PUBLIC Qt6::Gui)
