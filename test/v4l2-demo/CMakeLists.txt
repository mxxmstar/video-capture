find_package(glad CONFIG REQUIRED)

add_executable(v4l2_demo v4l2.cpp)
target_link_libraries(v4l2_demo PRIVATE render_lib glad::glad)
set_target_properties(v4l2_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
copy_runtime_dependencies(v4l2_demo)