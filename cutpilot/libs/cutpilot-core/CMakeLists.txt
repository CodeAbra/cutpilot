add_library(cutpilot-core STATIC
    src/NodeGraph.cpp
    src/SpatialIndex.cpp
    src/command/CommandStack.cpp
    src/command/AddNodeCommand.cpp
    src/command/MoveNodesCommand.cpp
    src/command/DeleteNodesCommand.cpp)

target_include_directories(cutpilot-core PUBLIC include)

target_link_libraries(cutpilot-core PUBLIC Qt6::Core Qt6::Gui)
