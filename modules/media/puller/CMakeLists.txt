media_module(media_input src/generic/ffmpeg_puller.cpp)
target_link_libraries(media_input PUBLIC media_ffmpeg media_diagnostics)
add_library(Media::Input ALIAS media_input)
