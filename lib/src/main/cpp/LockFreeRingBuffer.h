#ifndef LOCKFREERINGBUFFER_H
#define LOCKFREERINGBUFFER_H

#include <atomic>
#include <memory>
#include <cstring>
#include <cmath>

/**
 * Lock-free single-producer single-consumer ring buffer for real-time audio processing.
 * Safe for use in audio callbacks without blocking or priority inversion.
 */
template<typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(size_t capacity) :
        capacity_(nextPowerOfTwo(capacity)),
        mask_(capacity_ - 1),
        buffer_(std::make_unique<T[]>(capacity_)),
        writeIndex_(0),
        readIndex_(0) {
        // Zero-initialize the buffer
        std::memset(buffer_.get(), 0, capacity_ * sizeof(T));
    }

    ~LockFreeRingBuffer() = default;

    // Non-copyable, non-movable for safety
    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer(LockFreeRingBuffer&&) = delete;
    LockFreeRingBuffer& operator=(LockFreeRingBuffer&&) = delete;

    /**
     * Write data to the ring buffer (producer side)
     * @param data Pointer to data to write
     * @param count Number of elements to write
     * @return Number of elements actually written
     */
    size_t write(const T* data, size_t count) {
        const size_t currentWrite = writeIndex_.load(std::memory_order_relaxed);
        const size_t currentRead = readIndex_.load(std::memory_order_acquire);

        const size_t available = capacity_ - ((currentWrite - currentRead) & mask_);
        const size_t toWrite = std::min(count, available - 1); // Leave one slot empty for distinction

        if (toWrite == 0) {
            return 0;
        }

        // Write in two parts if wrapping around
        const size_t firstPart = std::min(toWrite, capacity_ - (currentWrite & mask_));
        std::memcpy(&buffer_[currentWrite & mask_], data, firstPart * sizeof(T));

        if (toWrite > firstPart) {
            const size_t secondPart = toWrite - firstPart;
            std::memcpy(&buffer_[0], &data[firstPart], secondPart * sizeof(T));
        }

        // Update write index with release semantics
        writeIndex_.store(currentWrite + toWrite, std::memory_order_release);
        return toWrite;
    }

    /**
     * Read data from the ring buffer (consumer side)
     * @param data Pointer to buffer to read into
     * @param count Number of elements to read
     * @return Number of elements actually read
     */
    size_t read(T* data, size_t count) {
        const size_t currentRead = readIndex_.load(std::memory_order_relaxed);
        const size_t currentWrite = writeIndex_.load(std::memory_order_acquire);

        const size_t available = (currentWrite - currentRead) & mask_;
        const size_t toRead = std::min(count, available);

        if (toRead == 0) {
            return 0;
        }

        // Read in two parts if wrapping around
        const size_t firstPart = std::min(toRead, capacity_ - (currentRead & mask_));
        std::memcpy(data, &buffer_[currentRead & mask_], firstPart * sizeof(T));

        if (toRead > firstPart) {
            const size_t secondPart = toRead - firstPart;
            std::memcpy(&data[firstPart], &buffer_[0], secondPart * sizeof(T));
        }

        // Update read index with release semantics
        readIndex_.store(currentRead + toRead, std::memory_order_release);
        return toRead;
    }

    /**
     * Get the number of elements available for reading
     */
    size_t availableForRead() const {
        const size_t currentRead = readIndex_.load(std::memory_order_relaxed);
        const size_t currentWrite = writeIndex_.load(std::memory_order_acquire);
        return (currentWrite - currentRead) & mask_;
    }

    /**
     * Get the number of elements available for writing
     */
    size_t availableForWrite() const {
        const size_t currentWrite = writeIndex_.load(std::memory_order_relaxed);
        const size_t currentRead = readIndex_.load(std::memory_order_acquire);
        return capacity_ - ((currentWrite - currentRead) & mask_) - 1;
    }

    /**
     * Clear the buffer
     */
    void clear() {
        readIndex_.store(writeIndex_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    /**
     * Get the total capacity of the buffer
     */
    size_t capacity() const {
        return capacity_ - 1; // One slot is reserved for empty/full distinction
    }

private:
    static size_t nextPowerOfTwo(size_t n) {
        if (n == 0) return 1;
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        if constexpr (sizeof(size_t) > 4) {
            n |= n >> 32;
        }
        return n + 1;
    }

    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<T[]> buffer_;

    // Atomic indices with appropriate memory ordering
    std::atomic<size_t> writeIndex_;
    std::atomic<size_t> readIndex_;
};

// Type alias for float audio samples
using AudioRingBuffer = LockFreeRingBuffer<float>;

#endif // LOCKFREERINGBUFFER_H