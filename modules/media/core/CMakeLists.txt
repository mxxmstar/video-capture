# Shared configuration validation remains in the contract library.
add_library(media_core STATIC
    src/media_frame.cpp src/media_timestamp.cpp
    ../encoder/src/encoder_config.cpp
    ../pusher/src/pusher_config.cpp
    ../publisher/src/publisher_config.cpp)
add_library(Media::Core ALIAS media_core)
target_link_libraries(media_core PUBLIC Media::Contracts)
target_compile_features(media_core PUBLIC cxx_std_20)
target_compile_definitions(media_core PUBLIC RAW_FRAME_BUFFER=$<BOOL:${MEDIA_RAW_FRAME_BUFFER}>)
