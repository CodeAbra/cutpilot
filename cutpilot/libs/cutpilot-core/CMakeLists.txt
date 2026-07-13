add_library(cutpilot-core STATIC
    src/NodeGraph.cpp
    src/SpatialIndex.cpp
    src/AlignmentGuides.cpp
    src/ConnectorPath.cpp
    src/PortRules.cpp
    src/command/CommandStack.cpp
    src/command/AddNodeCommand.cpp
    src/command/MoveNodesCommand.cpp
    src/command/DeleteNodesCommand.cpp
    src/command/ConnectCommand.cpp
    src/command/DisconnectCommand.cpp
    src/command/CompositeCommand.cpp
    src/command/AddConnectedNodeCommand.cpp
    src/command/EditPromptCommand.cpp
    src/command/SetModelCommand.cpp
    src/command/SetGateLimitCommand.cpp
    src/command/SetCompositeParamsCommand.cpp
    src/command/SetMediaPathCommand.cpp
    src/CompositeNodes.cpp
    src/CompositePlan.cpp
    src/PipelineOrder.cpp
    src/ComfyImport.cpp
    src/timeline/Timeline.cpp
    src/timeline/EdlWriter.cpp
    src/timeline/GraphTimeline.cpp
    src/timeline/ProjectStore.cpp
    src/timeline/ExportBundle.cpp)

target_include_directories(cutpilot-core PUBLIC include)

target_link_libraries(cutpilot-core PUBLIC Qt6::Core Qt6::Gui)
