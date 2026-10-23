find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_someip tst_someip.cpp)
target_link_libraries(tst_someip PRIVATE sdv::someip Qt6::Test)
add_test(NAME someip_codec COMMAND tst_someip)

qt_add_executable(tst_ivshm tst_ivshm.cpp)
target_link_libraries(tst_ivshm PRIVATE sdv::ivshm Qt6::Test)
add_test(NAME ivshm_channel COMMAND tst_ivshm)

if(SDV_BUILD_OTA_AGENT)
    qt_add_executable(tst_ota_verify tst_ota_verify.cpp
        ${PROJECT_SOURCE_DIR}/ota-agent/src/manifest.cpp
        ${PROJECT_SOURCE_DIR}/ota-agent/src/signature.cpp)
    target_include_directories(tst_ota_verify PRIVATE ${PROJECT_SOURCE_DIR}/ota-agent/src)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(tst_ota_verify PRIVATE Qt6::Test Qt6::Core OpenSSL::Crypto)
    add_test(NAME ota_manifest_verification COMMAND tst_ota_verify)
endif()
