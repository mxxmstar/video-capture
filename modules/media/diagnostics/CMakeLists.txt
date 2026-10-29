media_module(media_diagnostics src/media_diagnostics.cpp)
target_link_libraries(media_diagnostics PUBLIC spdlog::spdlog)
target_include_directories(media_diagnostics PRIVATE "${EMBEDDED_SOURCE_ROOT}/include")
