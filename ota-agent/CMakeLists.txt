find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL 3 REQUIRED)

qt_add_executable(sdv-ota-agent
    src/main.cpp
    src/otaagent.h src/otaagent.cpp
    src/manifest.h src/manifest.cpp
    src/signature.h src/signature.cpp
    src/slotmanager.h src/slotmanager.cpp
    src/someipprovider.h src/someipprovider.cpp
)
target_compile_definitions(sdv-ota-agent PRIVATE SDV_SHELL_VERSION="${PROJECT_VERSION}")
target_link_libraries(sdv-ota-agent PRIVATE sdv::someip Qt6::Core Qt6::Network OpenSSL::Crypto)

install(TARGETS sdv-ota-agent RUNTIME DESTINATION bin)
