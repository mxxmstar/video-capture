find_package(OpenGL REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

file(GLOB_RECURSE RENDER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(render_lib STATIC ${RENDER_SOURCES})
target_link_libraries(render_lib
    PUBLIC common_lib Media::All
    PRIVATE OpenGL::GL glfw glad::glad)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(OpenGL QUIET COMPONENTS EGL)
    if(TARGET OpenGL::EGL)
        target_link_libraries(render_lib PRIVATE OpenGL::EGL)
        target_compile_definitions(render_lib PRIVATE RENDER_EGL_DMABUF=1)
    endif()
endif()

target_compile_options(render_lib PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>)

if(BUILD_TESTS)
    add_subdirectory(test)
    if(BUILD_V4L2_CAPTURE)
        add_subdirectory("${EMBEDDED_SOURCE_ROOT}/modules/media/capture/test/v4l2-demo"
            "${CMAKE_CURRENT_BINARY_DIR}/v4l2-demo")
    endif()
endif()
