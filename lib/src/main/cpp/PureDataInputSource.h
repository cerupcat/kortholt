#ifndef PUREDATAINPUTSOURCE_H
#define PUREDATAINPUTSOURCE_H

#include <cstdint>
#include <memory>
#include <atomic>
#include <array>
#include "LockFreeRingBuffer.h"

// Include libpd C headers for global functions
extern "C" {
    #include "z_libpd.h"
}

// Include Oboe IRenderableAudio interface
#include "./oboe/samples/shared/IRenderableAudio.h"

/**
 * Real-time safe Pure Data input source using lock-free ring buffers.
 * Eliminates mutex usage and memory allocation from audio callbacks.
 */
class PureDataInputSource : public IRenderableAudio {
private:
    // Configuration
    static constexpr size_t MAX_CHANNELS = 8;
    static constexpr size_t RING_BUFFER_SIZE = 8192; // Power of 2 for efficiency

    // Audio parameters
    int32_t ticksPerBuffer_;
    std::atomic<int32_t> inputChannels_{0};
    std::atomic<int32_t> sampleRate_{44100};
    std::atomic<bool> initialized_{false};

    // Per-channel ring buffers for lock-free audio data exchange
    std::array<std::unique_ptr<AudioRingBuffer>, MAX_CHANNELS> ringBuffers_;

    // Pre-allocated working buffers to avoid allocation in audio callbacks
    std::unique_ptr<float[]> tempBuffer_;
    size_t tempBufferSize_;

    // Statistics for monitoring (atomic for thread safety)
    std::atomic<uint64_t> totalFramesReceived_{0};
    std::atomic<uint64_t> droppedFrames_{0};

public:
    explicit PureDataInputSource(int32_t ticksPerBuffer);
    virtual ~PureDataInputSource() = default;

    // Non-copyable, non-movable for safety
    PureDataInputSource(const PureDataInputSource&) = delete;
    PureDataInputSource& operator=(const PureDataInputSource&) = delete;
    PureDataInputSource(PureDataInputSource&&) = delete;
    PureDataInputSource& operator=(PureDataInputSource&&) = delete;

    /**
     * Initialize the input source with audio parameters
     * This must be called before any audio processing
     */
    bool init(int32_t sampleRate, int32_t channelCount);

    /**
     * IRenderableAudio interface implementation - receives input audio data
     * This runs in the real-time audio input thread and must be lock-free
     */
    void renderAudio(float *audioData, int32_t numFrames) override;

    /**
     * Get the captured input data for Pure Data processing
     * This is called from the Pure Data processing thread
     */
    size_t getInputAudio(float *outputBuffer, int32_t maxFrames);

    /**
     * Check if input source is properly initialized
     */
    bool isInitialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

    /**
     * Get the number of input channels
     */
    int32_t getChannelCount() const {
        return inputChannels_.load(std::memory_order_acquire);
    }

    /**
     * Check how much audio data is available for reading
     */
    size_t availableFrames() const;

    /**
     * Get performance statistics
     */
    struct Statistics {
        uint64_t totalFramesReceived;
        uint64_t droppedFrames;
        double dropoutPercentage;
    };
    Statistics getStatistics() const;

    /**
     * Reset statistics counters
     */
    void resetStatistics();

    /**
     * Clear all audio buffers
     */
    void clearBuffers();

private:
    /**
     * Safely bounds-check parameters
     */
    bool validateParameters(int32_t sampleRate, int32_t channelCount) const;

    /**
     * Initialize ring buffers for the specified channel count
     */
    void initializeRingBuffers(int32_t channelCount);
};

#endif // PUREDATAINPUTSOURCE_H