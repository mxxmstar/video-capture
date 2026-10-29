#include "media/capture/frame_lease.h"
#include "media/media_buffer.h"
#include "media/capture/capture_queue.h"

FrameLease::~FrameLease() { 
    queue_->release(index_, generation_); 
}

FrameLease::FrameLease(std::shared_ptr<CaptureQueue> queue, std::shared_ptr<MmapBufferPool> storage,
        unsigned index, std::uint64_t generation, std::vector<MediaPlaneView> planeviews) 
        : queue_(std::move(queue)), storage_(std::move(storage)), index_(index), generation_(generation),
        view_(std::move(planeviews)) {

    }