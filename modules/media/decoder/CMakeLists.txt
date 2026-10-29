media_module(media_decoder src/generic/ffmpeg_decoder.cpp)
target_link_libraries(media_decoder PUBLIC media_converter)
add_library(Media::Decoder ALIAS media_decoder)
