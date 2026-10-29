#pragma once

#include "media/media_packet.h"
extern "C" {
#include <libavcodec/packet.h>
}

namespace Media {
struct AVPacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

// Consumes packet, including if shared_ptr control-block allocation fails.
inline std::shared_ptr<AVPacket> OwnAVPacket(AVPacket* packet) {
    return std::shared_ptr<AVPacket>(packet, AVPacketDeleter{});
}

/// @brief 绑定AVPacket到MediaPacket
/// @param target 目标MediaPacket
/// @param owner AVPacket的所有者
inline void BindAVPacket(MediaPacket& target, std::shared_ptr<AVPacket> owner) {
    if (!owner || owner->size < 0 || (owner->size != 0 && !owner->data)) {
        throw std::invalid_argument("Invalid AVPacket");
    }
    auto buffer = MediaPacketBuffer::Wrap(owner->data, static_cast<std::size_t>(owner->size), owner);
    target.buffer = std::move(buffer);
    target.backend = {BackendHandle::FFMPEG, owner.get()};
}

/// @brief 从 MediaPacket 获取 AVPacket 指针
/// @param packet MediaPacket
/// @return AVPacket 指针
inline const AVPacket* GetBackendPacket(const MediaPacket& packet) noexcept {
    if (!packet.buffer || packet.backend.type != BackendHandle::FFMPEG || !packet.backend.ptr || packet.buffer->Owner().get() != packet.backend.ptr) {
        return nullptr;
    }
    const auto* backend = static_cast<const AVPacket*>(packet.backend.ptr);
    if (backend->size <= 0 || !backend->data || backend->data != packet.buffer->Data() ||
        static_cast<std::size_t>(backend->size) != packet.buffer->Size()) return nullptr;
    return backend;
}
} // namespace Media
