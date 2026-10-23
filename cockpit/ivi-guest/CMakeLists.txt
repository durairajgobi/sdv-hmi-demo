find_package(Qt6 REQUIRED COMPONENTS Gui)
qt_add_executable(sdv-ivi-guest src/main.cpp)
target_link_libraries(sdv-ivi-guest PRIVATE sdv::ivshm Qt6::Gui)
install(TARGETS sdv-ivi-guest RUNTIME DESTINATION bin)
