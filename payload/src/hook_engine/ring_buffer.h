#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

namespace positron::hook {

// Single-producer / single-consumer lock-free ring buffer for variable-size
// records. Each record is prefixed with a 4-byte length. Used to buffer
// hook-hit events from the detour thread (producer, must not block) into
// the V8 thread (consumer, drains during async_cb).
//
// Capacity must be a power of two. Records up to (Capacity - 4) bytes.
template <size_t Capacity>
struct RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    bool push(const void* data, uint32_t len) noexcept {
        uint32_t wr = write_.load(std::memory_order_relaxed);
        uint32_t rd = read_.load(std::memory_order_acquire);
        uint32_t free_bytes = Capacity - (wr - rd);
        if (free_bytes < len + 4) return false;
        write_at(wr, &len, 4);
        write_at(wr + 4, data, len);
        write_.store(wr + 4 + len, std::memory_order_release);
        return true;
    }

    // Returns 0 if empty; otherwise copies up to dst_cap bytes and returns the
    // record's true size (which may exceed dst_cap if the caller's buffer was
    // too small — caller should size for the largest expected record).
    uint32_t pop(void* dst, uint32_t dst_cap) noexcept {
        uint32_t rd = read_.load(std::memory_order_relaxed);
        uint32_t wr = write_.load(std::memory_order_acquire);
        if (wr == rd) return 0;
        uint32_t len = 0;
        read_at(rd, &len, 4);
        uint32_t copy = (len < dst_cap) ? len : dst_cap;
        read_at(rd + 4, dst, copy);
        read_.store(rd + 4 + len, std::memory_order_release);
        return len;
    }

private:
    std::atomic<uint32_t> read_{0};
    std::atomic<uint32_t> write_{0};
    uint8_t buf_[Capacity]{};

    void write_at(uint32_t off, const void* src, uint32_t n) noexcept {
        off &= (Capacity - 1);
        uint32_t first = (n < Capacity - off) ? n : Capacity - off;
        std::memcpy(buf_ + off, src, first);
        if (first < n) std::memcpy(buf_, static_cast<const uint8_t*>(src) + first, n - first);
    }
    void read_at(uint32_t off, void* dst, uint32_t n) noexcept {
        off &= (Capacity - 1);
        uint32_t first = (n < Capacity - off) ? n : Capacity - off;
        std::memcpy(dst, buf_ + off, first);
        if (first < n) std::memcpy(static_cast<uint8_t*>(dst) + first, buf_, n - first);
    }
};

} // namespace positron::hook
