#ifndef PUREDATAINPUTSOURCE_H
#define PUREDATAINPUTSOURCE_H

#include <cstdint>
#include <memory>
#include <atomic>
#include <array>
#include "LockFreeRingBuffer.h"
#include "AudioRecorderCallback.h"

// Include libpd C headers for global functions
extern "C" {
    #include "z_libpd.h"
}

// Include Oboe IRenderableAudio interface
#include <IRenderableAudio.h>

/**
 * Real-time safe Pure Data input source using lock-free ring buffers.
 * Eliminates mutex usage and memory allocation from audio callbacks.
 */
class PureDataInputSource : public IRenderableAudio {
private:
    // Configuration
    static constexpr size_t MAX_CHANNELS = 8;
    static constexpr size_t MIN_RING_BUFFER_SIZE = 2048;  // Minimum for low-end devices
    static constexpr size_t MAX_RING_BUFFER_SIZE = 16384; // Maximum for high-end devices
    static constexpr size_t DEFAULT_RING_BUFFER_SIZE = 8192; // Default fallback

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

    // Adaptive buffer sizing
    size_t adaptiveRingBufferSize_;

    // Statistics for monitoring (atomic for thread safety)
    std::atomic<uint64_t> totalFramesReceived_{0};
    std::atomic<uint64_t> droppedFrames_{0};

    // Silence detection for InputPreset fallback (atomic for thread safety)
    // Tracks consecutive all-zero callbacks to detect digital silence from buggy
    // AAudio InputPreset handling on Samsung/MediaTek devices.
    std::atomic<uint64_t> consecutiveZeroCallbacks_{0};
    std::atomic<uint64_t> totalInputCallbacks_{0};

    // Optional recorder callback (atomic pointer for thread-safe updates)
    std::atomic<AudioRecorderCallback*> recorderCallback_{nullptr};

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
     * Check if the input stream is producing digital silence.
     * Digital silence = all-zero samples from the hardware, indicating
     * the device's AAudio implementation is not delivering audio data.
     *
     * @param graceCallbacks Number of callbacks to skip after stream start
     *                       (allows hardware warmup)
     * @param silenceCallbacks Number of consecutive all-zero callbacks
     *                         required to confirm digital silence
     * @return true if past grace period AND enough consecutive zero callbacks
     */
    bool isDigitalSilence(uint64_t graceCallbacks, uint64_t silenceCallbacks) const;

    /**
     * Reset silence detection counters.
     * Call when reopening the input stream with a new configuration.
     */
    void resetSilenceCounters();

    /**
     * Clear all audio buffers
     */
    void clearBuffers();

    /**
     * Set a recorder callback to receive audio data for recording.
     * The callback will be invoked from the audio thread with raw PCM data.
     *
     * @param callback Recorder callback (or nullptr to remove)
     */
    void setRecorderCallback(AudioRecorderCallback* callback) {
        recorderCallback_.store(callback, std::memory_order_release);
    }

private:
    /**
     * Safely bounds-check parameters
     */
    bool validateParameters(int32_t sampleRate, int32_t channelCount) const;

    /**
     * Initialize ring buffers for the specified channel count
     */
    void initializeRingBuffers(int32_t channelCount);

    /**
     * Calculate optimal ring buffer size based on device capabilities
     */
    size_t calculateOptimalBufferSize(int32_t sampleRate, int32_t channelCount) const;
};

#endif // PUREDATAINPUTSOURCE_H
