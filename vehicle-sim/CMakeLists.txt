add_executable(vehicle-sim
    src/main.cpp
    src/udp.h
    src/vehicle_model.h
)
target_link_libraries(vehicle-sim PRIVATE sdv::someip)
install(TARGETS vehicle-sim RUNTIME DESTINATION bin)
