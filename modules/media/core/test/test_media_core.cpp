#include "media/media_timestamp.h"
#include "media/converter/i_frame_converter.h"
#include "media/decoder/i_decoder.h"
#include "media/encoder/i_encoder.h"
#include "media/puller/i_puller.h"
#include "media/pusher/i_pusher.h"
#include <limits>
#include <iostream>
int main() {
    const auto max = (std::numeric_limits<std::int64_t>::max)();
    if (RescaleMediaTimestamp(1,{1,2},{1,1}) != 1 ||
        RescaleMediaTimestamp(-1,{1,2},{1,1}) != -1 ||
        RescaleMediaTimestamp(max,{1,1},{1,1}) != max ||
        RescaleMediaTimestamp(max,{2,1},{1,1}) != kNoTimestamp ||
        RescaleMediaTimestamp(kNoTimestamp,{1,1},{1,1}) != kNoTimestamp ||
        RescaleMediaTimestamp(1,{0,1},{1,1}) != kNoTimestamp) return 1;
    EncoderConfig config; config.video().width=16; config.video().height=16;
    if (!config.is_valid()) return 2;
    const std::uint8_t bytes[]{1,2,3};
    MediaPacket packet; packet.buffer = std::make_shared<MediaPacketBuffer>(bytes,3);
    auto held = packet; packet.buffer.reset();
    if (held.buffer->Data()[2] != 3) return 3;
    std::cout << "Core contracts passed without external SDKs\n";
}
