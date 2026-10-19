#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gm {

/// Simple bump allocator. Memory is freed only by `reset()` or destruction,
/// which is exactly what per-level asset loading needs.
class Arena {
public:
    explicit Arena(std::size_t capacity) : buffer_(capacity) {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* alloc(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) {
        if (alignment == 0) {
            alignment = 1;
        }
        const std::size_t aligned = (used_ + alignment - 1) & ~(alignment - 1);
        if (aligned > buffer_.size() || size > buffer_.size() - aligned) {
            return nullptr;
        }
        void* ptr = buffer_.data() + aligned;
        used_ = aligned + size;
        return ptr;
    }

    template <class T>
    T* make(std::size_t count = 1) {
        return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
    }

    std::size_t used() const { return used_; }
    std::size_t capacity() const { return buffer_.size(); }
    std::size_t remaining() const { return buffer_.size() - used_; }
    void reset() { used_ = 0; }

private:
    std::vector<std::byte> buffer_;
    std::size_t used_ = 0;
};

} // namespace gm
