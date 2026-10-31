add_library(media_ffmpeg INTERFACE)
target_include_directories(media_ffmpeg INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backend/include>"
    ${FFMPEG_INCLUDE_DIR})
target_link_libraries(media_ffmpeg INTERFACE Media::Core ${FFMPEG_LIBS} spdlog::spdlog)
target_include_directories(media_ffmpeg INTERFACE "${EMBEDDED_SOURCE_ROOT}/include")

# Resolve transitive target DSOs in a board-exported SDK while cross linking.
# This is a link-time search path, not a deployed runtime library path.
if(CMAKE_CROSSCOMPILING AND FFMPEG_ROOT)
    target_link_options(media_ffmpeg INTERFACE "LINKER:-rpath-link,${FFMPEG_ROOT}/lib:${FFMPEG_ROOT}/link-deps")
endif()
