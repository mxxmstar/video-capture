#include "media/capture/mmap_buffer_pool.h"
#include <cerrno>
#include <sys/mman.h>
#include <stdexcept>

MmapBufferPool::~MmapBufferPool() {
    for (const auto &frameplanes : pool_) {
        for (const auto &mapping : frameplanes) {
            ::munmap(mapping.address, mapping.length);
        }
    }
}

void MmapBufferPool::ResizeSlots(std::size_t count) {
    if (!pool_.empty()) throw std::logic_error("buffer pool slots already initialized");
    pool_.resize(count);
}

int MmapBufferPool::Map(int fd, unsigned index, std::size_t length, std::uint32_t offset) {
    if (length == 0) return EINVAL;
    if (index >= pool_.size()) pool_.resize(static_cast<std::size_t>(index) + 1);
    void *address = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (index >= pool_.size() || length == 0) return EINVAL;
    if (address == MAP_FAILED) {
        return errno;
    }        
    try {
        pool_[index].push_back({address, length, offset});
    }
    catch (...) {
        ::munmap(address, length);
        throw;
    }
    return 0;
}

const MmapBufferPool::FramePlanes& MmapBufferPool::At(unsigned index) const {
    if (index >= pool_.size()) {
        throw std::out_of_range("index out of range");
    }
    return pool_[index];
}
