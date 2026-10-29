add_executable(test_capture_contract test_capture_contract.cpp)
target_link_libraries(test_capture_contract PRIVATE Media::All)
add_test(NAME test_capture_contract COMMAND test_capture_contract)

add_executable(v4l2_capture_smoke v4l2_capture_smoke.cpp)
target_link_libraries(v4l2_capture_smoke PRIVATE Media::All)
if(BUILD_RENDER)
    target_compile_definitions(v4l2_capture_smoke PRIVATE V4L2_SMOKE_RENDER=1)
    find_package(glad CONFIG REQUIRED)
    target_link_libraries(v4l2_capture_smoke PRIVATE render_lib glad::glad)
endif()
add_executable(v4l2_lease_test v4l2_lease_test.cpp)
target_link_libraries(v4l2_lease_test PRIVATE Media::All)
add_executable(v4l2_vivid_fault_test v4l2_vivid_fault_test.cpp)
target_link_libraries(v4l2_vivid_fault_test PRIVATE Media::All)
option(BUILD_V4L2_DEVICE_TESTS "Register tests that open a V4L2 device" OFF)
set(V4L2_TEST_DEVICE "/dev/video0" CACHE STRING "V4L2 device for explicit integration tests")
if(BUILD_V4L2_DEVICE_TESTS)
    foreach(format yuyv nv12 i420)
        add_test(NAME test_capture_lease_${format}
            COMMAND v4l2_lease_test "${V4L2_TEST_DEVICE}" ${format})
        set_tests_properties(test_capture_lease_${format} PROPERTIES
            TIMEOUT 30 RUN_SERIAL TRUE LABELS "v4l2;device")
        add_test(NAME test_capture_vivid_${format}
            COMMAND v4l2_capture_smoke "${V4L2_TEST_DEVICE}" ${format}
                "${CMAKE_CURRENT_BINARY_DIR}/vivid-${format}.mp4")
        set_tests_properties(test_capture_vivid_${format} PROPERTIES
            TIMEOUT 30 RUN_SERIAL TRUE LABELS "v4l2;device")
    endforeach()
endif()
option(BUILD_V4L2_VIVID_FAULT_TESTS "Register vivid-specific error injection tests" OFF)
if(BUILD_V4L2_VIVID_FAULT_TESTS)
    add_test(NAME test_capture_vivid_faults
        COMMAND v4l2_vivid_fault_test "${V4L2_TEST_DEVICE}")
    set_tests_properties(test_capture_vivid_faults PROPERTIES
        TIMEOUT 30 RUN_SERIAL TRUE LABELS "v4l2;device;vivid;fault")
endif()
