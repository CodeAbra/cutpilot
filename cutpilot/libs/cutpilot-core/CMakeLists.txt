add_library(cutpilot-core STATIC
    src/NodeGraph.cpp)

target_include_directories(cutpilot-core PUBLIC include)

target_link_libraries(cutpilot-core PUBLIC Qt6::Core Qt6::Gui)
