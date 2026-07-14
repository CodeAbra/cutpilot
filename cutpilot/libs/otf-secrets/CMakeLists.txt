add_library(otf-secrets STATIC
    src/KeychainStore.cpp
    src/SecretStore.cpp)

target_include_directories(otf-secrets PUBLIC include)

target_link_libraries(otf-secrets PUBLIC Qt6::Core)

if(APPLE)
    target_link_libraries(otf-secrets PRIVATE
        "-framework Security"
        "-framework CoreFoundation")
endif()
