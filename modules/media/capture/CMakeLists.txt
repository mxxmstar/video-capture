media_module(media_capture src/capture_queue.cpp src/frame_lease.cpp src/mmap_buffer_pool.cpp src/v4l2_video_capture.cpp src/video_capture_session.cpp)
target_link_libraries(media_capture PUBLIC spdlog::spdlog Threads::Threads)
add_library(Media::Capture ALIAS media_capture)
target_include_directories(media_capture PRIVATE "${EMBEDDED_SOURCE_ROOT}/include")
