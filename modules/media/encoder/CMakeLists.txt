media_module(media_encoder src/generic/ffmpeg_encoder.cpp)
target_link_libraries(media_encoder PUBLIC media_converter)
add_library(Media::Encoder ALIAS media_encoder)
