add_executable(sdv-hypervisor src/main.cpp src/config.h src/config.cpp)
target_link_libraries(sdv-hypervisor PRIVATE sdv::ivshm)
install(TARGETS sdv-hypervisor RUNTIME DESTINATION bin)
