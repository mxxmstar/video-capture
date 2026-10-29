add_library(common_lib STATIC ${EMBEDDED_PROCESS_SOURCE})
target_include_directories(common_lib PUBLIC
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(common_lib PUBLIC Embedded::Platform)
target_compile_options(common_lib PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic>)
if(MSVC)
    target_compile_options(common_lib PUBLIC /utf-8 /Zc:__cplusplus)
endif()

if(BUILD_TESTS)
    add_subdirectory(process/test)
endif()
