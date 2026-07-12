# ============================================================
# Test targets
# ============================================================

# spdlog 测试
add_executable(test_spdlog test_spdlog.cpp)
target_link_libraries(test_spdlog spdlog)
if(WIN32)
    target_link_libraries(test_spdlog ws2_32)
else()
    target_link_libraries(test_spdlog pthread)
endif()

# 拉流解码测试
add_executable(test_stream_decode test_stream_decode.cpp)
target_link_libraries(test_stream_decode
    video_capture_lib
)

# 将 FFmpeg DLL 复制到测试可执行文件输出目录
if(WIN32)
    file(GLOB FFMPEG_DLLS "${CMAKE_BINARY_DIR}/*.dll")
    add_custom_command(TARGET test_stream_decode POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_DLLS}
            $<TARGET_FILE_DIR:test_stream_decode>
        COMMENT "Copying FFmpeg DLLs to test output directory"
    )
endif()