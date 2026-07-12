qt_add_library(cutpilot-ipc STATIC
    include/cutpilot/ipc/GenerationTypes.h
    include/cutpilot/ipc/GenerationClient.h
    include/cutpilot/ipc/SidecarHost.h
    include/cutpilot/ipc/GenerationCoordinator.h
    src/GenerationClient.cpp
    src/SidecarHost.cpp
    src/GenerationCoordinator.cpp)

target_include_directories(cutpilot-ipc PUBLIC include)

target_link_libraries(cutpilot-ipc
    PUBLIC
        cutpilot-core
        Qt6::Network
    PRIVATE
        Qt6::Concurrent)
