add_library(media_ffmpeg INTERFACE)
target_include_directories(media_ffmpeg INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/backend/include>"
    ${FFMPEG_INCLUDE_DIR})
target_link_libraries(media_ffmpeg INTERFACE Media::Core ${FFMPEG_LIBS} spdlog::spdlog)
target_include_directories(media_ffmpeg INTERFACE "${EMBEDDED_SOURCE_ROOT}/include")
