#include "media/pusher/pusher_factory.h"
#include "media/pusher/ffmpeg/ffmpeg_pusher.h"
std::unique_ptr<IPusher> CreateMediaPusher() { return std::make_unique<FFmpegPusher>(); }
