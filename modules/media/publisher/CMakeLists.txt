media_module(media_publisher src/publisher.cpp)
target_link_libraries(media_publisher PUBLIC media_pusher_session)
add_library(Media::Publisher ALIAS media_publisher)
