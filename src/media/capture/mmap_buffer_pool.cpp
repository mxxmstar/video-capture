#include "media/capture/mmap_buffer_pool.h"
#include <cerrno>
#include <sys/mman.h>

MmapBufferPool::~MmapBufferPool() {
    for (const auto &mapping : mappings_) {
        ::munmap(mapping.address, mapping.length);
    }
}

int MmapBufferPool::Map(int fd, std::size_t length, std::uint32_t offset) {
    void *address = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (address == MAP_FAILED) {
        return errno;
    }        
    try {
        mappings_.push_back({address, length});
    }
    catch (...) {
        ::munmap(address, length);
        throw;
    }
    return 0;
}
