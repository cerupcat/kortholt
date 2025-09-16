#include "PureDataInputSource.h"
#include <android/log.h>
#include <cstring>
#include <algorithm>

#define LOG_TAG "PureDataInputSource"
#ifndef LOGD
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGW
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#endif

PureDataInputSource::PureDataInputSource(int32_t ticksPerBuffer) :
    ticksPerBuffer_(ticksPerBuffer),
    tempBufferSize_(0) {
    LOGD("PureDataInputSource created with ticksPerBuffer=%d", ticksPerBuffer);

    // Pre-calculate maximum buffer size we might need
    const size_t maxFramesPerCallback = ticksPerBuffer_ * libpd_blocksize();
    tempBufferSize_ = maxFramesPerCallback * MAX_CHANNELS;
    tempBuffer_ = std::make_unique<float[]>(tempBufferSize_);

    // Zero-initialize temp buffer
    std::memset(tempBuffer_.get(), 0, tempBufferSize_ * sizeof(float));

    LOGD("PureDataInputSource initialized with temp buffer size: %zu frames", tempBufferSize_);
}

bool PureDataInputSource::validateParameters(int32_t sampleRate, int32_t channelCount) const {
    if (sampleRate < 8000 || sampleRate > 192000) {
        LOGE("Invalid sample rate: %d (must be between 8000-192000)", sampleRate);
        return false;
    }

    if (channelCount < 1 || channelCount > static_cast<int32_t>(MAX_CHANNELS)) {
        LOGE("Invalid channel count: %d (must be between 1-%zu)", channelCount, MAX_CHANNELS);
        return false;
    }

    return true;
}

void PureDataInputSource::initializeRingBuffers(int32_t channelCount) {
    for (int32_t i = 0; i < channelCount; ++i) {
        ringBuffers_[i] = std::make_unique<AudioRingBuffer>(RING_BUFFER_SIZE);
        LOGD("Initialized ring buffer for channel %d", i);
    }

    // Clear unused channels
    for (size_t i = channelCount; i < MAX_CHANNELS; ++i) {
        ringBuffers_[i].reset();
    }
}

bool PureDataInputSource::init(int32_t sampleRate, int32_t channelCount) {
    LOGD("Initializing PureDataInputSource: sampleRate=%d, channelCount=%d, ticksPerBuffer=%d",
         sampleRate, channelCount, ticksPerBuffer_);

    if (!validateParameters(sampleRate, channelCount)) {
        return false;
    }

    // Store parameters atomically
    sampleRate_.store(sampleRate, std::memory_order_release);
    inputChannels_.store(channelCount, std::memory_order_release);

    // Initialize ring buffers
    initializeRingBuffers(channelCount);

    // Reset statistics
    resetStatistics();

    // Mark as initialized (this must be last)
    initialized_.store(true, std::memory_order_release);

    LOGD("PureDataInputSource initialization complete");
    return true;
}

void PureDataInputSource::renderAudio(float *audioData, int32_t numFrames) {
    // This method runs in the real-time audio input thread
    // It MUST be lock-free and allocation-free

    if (!initialized_.load(std::memory_order_acquire)) {
        return;
    }

    const int32_t channels = inputChannels_.load(std::memory_order_acquire);
    if (channels <= 0 || !audioData) {
        return;
    }

    // Update statistics
    totalFramesReceived_.fetch_add(numFrames, std::memory_order_relaxed);

    // Validate frame count to prevent buffer overruns
    const int32_t maxAllowedFrames = static_cast<int32_t>(tempBufferSize_ / channels);
    const int32_t framesToProcess = std::min(numFrames, maxAllowedFrames);

    if (framesToProcess != numFrames) {
        LOGW("Frame count clamped from %d to %d to prevent buffer overrun", numFrames, framesToProcess);
        droppedFrames_.fetch_add(numFrames - framesToProcess, std::memory_order_relaxed);
    }

    // Write interleaved audio data to per-channel ring buffers
    size_t totalDropped = 0;

    for (int32_t frame = 0; frame < framesToProcess; ++frame) {
        for (int32_t ch = 0; ch < channels; ++ch) {
            const float sample = audioData[frame * channels + ch];

            // Try to write sample to ring buffer
            const size_t written = ringBuffers_[ch]->write(&sample, 1);
            if (written != 1) {
                ++totalDropped;
            }
        }
    }

    if (totalDropped > 0) {
        droppedFrames_.fetch_add(totalDropped / channels, std::memory_order_relaxed);
    }
}

size_t PureDataInputSource::getInputAudio(float *outputBuffer, int32_t maxFrames) {
    if (!initialized_.load(std::memory_order_acquire) || !outputBuffer || maxFrames <= 0) {
        return 0;
    }

    const int32_t channels = inputChannels_.load(std::memory_order_acquire);
    if (channels <= 0) {
        return 0;
    }

    // Find minimum available frames across all channels
    size_t availableFrames = SIZE_MAX;
    for (int32_t ch = 0; ch < channels; ++ch) {
        if (ringBuffers_[ch]) {
            availableFrames = std::min(availableFrames, ringBuffers_[ch]->availableForRead());
        }
    }

    if (availableFrames == SIZE_MAX) {
        availableFrames = 0;
    }

    const size_t framesToRead = std::min(static_cast<size_t>(maxFrames), availableFrames);
    if (framesToRead == 0) {
        // Zero-fill output buffer when no input is available
        std::memset(outputBuffer, 0, maxFrames * channels * sizeof(float));
        return 0;
    }

    // Read from ring buffers and interleave
    for (size_t frame = 0; frame < framesToRead; ++frame) {
        for (int32_t ch = 0; ch < channels; ++ch) {
            float sample = 0.0f;
            const size_t read = ringBuffers_[ch]->read(&sample, 1);

            // Use the sample if read successfully, otherwise use 0
            outputBuffer[frame * channels + ch] = (read == 1) ? sample : 0.0f;
        }
    }

    // Zero-fill remaining frames if we didn't read enough
    if (framesToRead < static_cast<size_t>(maxFrames)) {
        const size_t remainingFrames = maxFrames - framesToRead;
        const size_t remainingBytes = remainingFrames * channels * sizeof(float);
        std::memset(&outputBuffer[framesToRead * channels], 0, remainingBytes);
    }

    return framesToRead;
}

size_t PureDataInputSource::availableFrames() const {
    if (!initialized_.load(std::memory_order_acquire)) {
        return 0;
    }

    const int32_t channels = inputChannels_.load(std::memory_order_acquire);
    if (channels <= 0) {
        return 0;
    }

    size_t minAvailable = SIZE_MAX;
    for (int32_t ch = 0; ch < channels; ++ch) {
        if (ringBuffers_[ch]) {
            minAvailable = std::min(minAvailable, ringBuffers_[ch]->availableForRead());
        }
    }

    return (minAvailable == SIZE_MAX) ? 0 : minAvailable;
}

PureDataInputSource::Statistics PureDataInputSource::getStatistics() const {
    const uint64_t total = totalFramesReceived_.load(std::memory_order_acquire);
    const uint64_t dropped = droppedFrames_.load(std::memory_order_acquire);

    Statistics stats;
    stats.totalFramesReceived = total;
    stats.droppedFrames = dropped;
    stats.dropoutPercentage = (total > 0) ? (static_cast<double>(dropped) / total * 100.0) : 0.0;

    return stats;
}

void PureDataInputSource::resetStatistics() {
    totalFramesReceived_.store(0, std::memory_order_relaxed);
    droppedFrames_.store(0, std::memory_order_relaxed);
}

void PureDataInputSource::clearBuffers() {
    if (!initialized_.load(std::memory_order_acquire)) {
        return;
    }

    const int32_t channels = inputChannels_.load(std::memory_order_acquire);
    for (int32_t ch = 0; ch < channels; ++ch) {
        if (ringBuffers_[ch]) {
            ringBuffers_[ch]->clear();
        }
    }
}