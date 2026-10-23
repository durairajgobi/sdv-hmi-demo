add_library(someip_lite STATIC
    src/someip.cpp
    src/sd.cpp
    include/someip/someip.h
    include/someip/sd.h
    include/someip/services.h
)
add_library(sdv::someip ALIAS someip_lite)
target_include_directories(someip_lite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(someip_lite PROPERTIES POSITION_INDEPENDENT_CODE ON)
