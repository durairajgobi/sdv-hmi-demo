add_library(ivshm STATIC src/ivshm.cpp include/ivshm/ivshm.h)
add_library(sdv::ivshm ALIAS ivshm)
target_include_directories(ivshm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(ivshm PROPERTIES POSITION_INDEPENDENT_CODE ON)
