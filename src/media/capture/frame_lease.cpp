#include "media/capture/capture_queue.h"
#include <utility>

FrameLease::FrameLease(std::shared_ptr<CaptureQueue> queue, std::shared_ptr<MmapBufferPool> storage,
                       unsigned index, std::uint64_t generation, uint8_t* data, std::size_t size)
    : queue_(std::move(queue)), storage_(std::move(storage)), index_(index),
      generation_(generation), data_(data), size_(size) {}

FrameLease::~FrameLease() { 
  queue_->release(index_, generation_); 
}
