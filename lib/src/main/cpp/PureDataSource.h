#ifndef PUREDATASOURCE_H
#define PUREDATASOURCE_H

#include <cstdint>
#include <memory>
#include <atomic>
#include <array>
#include "PureDataInputSource.h"

// Include libpd C headers for global functions
extern "C" {
    #include "z_libpd.h"
}

// Include Oboe IRenderableAudio interface
#include "./oboe/samples/shared/IRenderableAudio.h"

/**
 * Real-time safe Pure Data source that eliminates mutex usage from audio callbacks.
 * Uses pre-allocated buffers and atomic operations for thread-safe communication.
 */
class PureDataSource : public IRenderableAudio {
private:
    // Audio processing configuration
    static constexpr size_t MAX_CHANNELS = 8;

    // Audio parameters (atomic for thread-safe access)
    int32_t ticksPerBuffer_;
    std::atomic<int32_t> inputChannels_{0};
    std::atomic<int32_t> outputChannels_{2};
    std::atomic<bool> initialized_{false};

    // Input source reference (set during initialization, immutable during processing)
    std::shared_ptr<PureDataInputSource> inputSource_;

    // Pre-allocated buffers for real-time processing (sized during initialization)
    std::unique_ptr<float[]> inputBuffer_;
    std::unique_ptr<float[]> tempBuffer_;
    size_t maxFramesPerCallback_;
    size_t inputBufferSize_;
    size_t tempBufferSize_;

    // Statistics (atomic for thread safety)
    std::atomic<uint64_t> totalCallbacks_{0};
    std::atomic<uint64_t> failedCallbacks_{0};

public:
    explicit PureDataSource(int32_t ticksPerBuffer);
    virtual ~PureDataSource() = default;

    // Non-copyable, non-movable for safety
    PureDataSource(const PureDataSource&) = delete;
    PureDataSource& operator=(const PureDataSource&) = delete;
    PureDataSource(PureDataSource&&) = delete;
    PureDataSource& operator=(PureDataSource&&) = delete;

    /**
     * Initialize Pure Data with audio parameters
     * This must be called before any audio processing
     */
    bool init(int32_t sampleRate, int32_t channelCount);

    /**
     * Set the number of input channels (must be called before init)
     */
    void setInputChannels(int32_t channels);

    /**
     * Set the input source for Pure Data processing (must be called before init)
     */
    void setInputSource(std::shared_ptr<PureDataInputSource> source);

    /**
     * IRenderableAudio interface - renders Pure Data output
     * This runs in the real-time audio output thread and must be lock-free
     */
    void renderAudio(float *audioData, int32_t numFrames) override;

    /**
     * Process audio with both input and output (for offline processing)
     * This is NOT real-time safe and should only be used for file processing
     */
    bool processAudio(float *inputData, float *outputData, int32_t numFrames);

    /**
     * Send messages to Pure Data (thread-safe)
     */
    void sendFloat(const char *dest, float value);
    void sendBang(const char *dest);
    void sendSymbol(const char *dest, const char *symbol);

    /**
     * Patch management (thread-safe)
     */
    bool openPatch(const char *patch, const char *path);
    void addToSearchPath(const char *path);

    /**
     * Check if initialized
     */
    bool isInitialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

    /**
     * Get performance statistics
     */
    struct Statistics {
        uint64_t totalCallbacks;
        uint64_t failedCallbacks;
        double failurePercentage;
    };
    Statistics getStatistics() const;

    /**
     * Reset statistics counters
     */
    void resetStatistics();

private:
    /**
     * Validate initialization parameters
     */
    bool validateParameters(int32_t sampleRate, int32_t channelCount) const;

    /**
     * Initialize pre-allocated buffers
     */
    bool initializeBuffers();

    /**
     * Calculate maximum frames per callback based on ticks
     */
    size_t calculateMaxFramesPerCallback() const;

    /**
     * Process Pure Data ticks safely
     */
    bool processPdTicks(int32_t numFrames, float *outputData);
};

#endif // PUREDATASOURCE_H