media_module(media_pipeline src/stream_session.cpp)
target_link_libraries(media_pipeline PUBLIC media_diagnostics Threads::Threads)
add_library(Media::Pipeline ALIAS media_pipeline)
target_include_directories(media_pipeline PRIVATE "${EMBEDDED_SOURCE_ROOT}/include")
