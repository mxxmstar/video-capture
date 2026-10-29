media_module(media_converter src/generic/ffmpeg_audio_converter.cpp src/generic/ffmpeg_video_converter.cpp src/generic/media_frame_converter.cpp)
target_link_libraries(media_converter PUBLIC media_ffmpeg media_diagnostics)
add_library(Media::Converter ALIAS media_converter)
