find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick Network)

qt_add_executable(sdv-cluster-hmi
    src/main.cpp
    src/someipclient.h src/someipclient.cpp
    src/vehiclestate.h src/vehiclestate.cpp
    src/otaclient.h src/otaclient.cpp
    src/hmishell.h src/hmishell.cpp
    src/cockpitlink.h src/cockpitlink.cpp
    src/ivisurfaceitem.h
)
target_compile_definitions(sdv-cluster-hmi PRIVATE SDV_SHELL_VERSION="${PROJECT_VERSION}")
target_link_libraries(sdv-cluster-hmi PRIVATE
    sdv::someip sdv::ivshm Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network)

install(TARGETS sdv-cluster-hmi RUNTIME DESTINATION bin)
