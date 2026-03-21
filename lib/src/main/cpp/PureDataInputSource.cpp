#include "PureDataInputSource.h"
#include <android/log.h>
#include <cstring>
#include <algorithm>
#include <sys/system_properties.h>
#include <unistd.h>

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
    tempBufferSize_(0),
    adaptiveRingBufferSize_(DEFAULT_RING_BUFFER_SIZE) {
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
        ringBuffers_[i] = std::make_unique<AudioRingBuffer>(adaptiveRingBufferSize_);
        LOGD("Initialized ring buffer for channel %d with size %zu", i, adaptiveRingBufferSize_);
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

    // Calculate optimal buffer size based on device capabilities
    adaptiveRingBufferSize_ = calculateOptimalBufferSize(sampleRate, channelCount);
    LOGD("Using adaptive ring buffer size: %zu (based on device capabilities)", adaptiveRingBufferSize_);

    // Store parameters atomically
    sampleRate_.store(sampleRate, std::memory_order_release);
    inputChannels_.store(channelCount, std::memory_order_release);

    // Initialize ring buffers with adaptive size
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

    // Forward audio data to recorder if one is attached
    AudioRecorderCallback* recorder = recorderCallback_.load(std::memory_order_acquire);
    if (recorder != nullptr) {
        recorder->onAudioData(audioData, numFrames, channels);
    }

    // Update statistics
    totalFramesReceived_.fetch_add(numFrames, std::memory_order_relaxed);
    totalInputCallbacks_.fetch_add(1, std::memory_order_relaxed);

    // Validate frame count to prevent buffer overruns
    const int32_t maxAllowedFrames = static_cast<int32_t>(tempBufferSize_ / channels);
    const int32_t framesToProcess = std::min(numFrames, maxAllowedFrames);

    if (framesToProcess != numFrames) {
        droppedFrames_.fetch_add(numFrames - framesToProcess, std::memory_order_relaxed);
    }

    // Write interleaved audio data to per-channel ring buffers
    size_t totalDropped = 0;
    bool allZero = true;

    for (int32_t frame = 0; frame < framesToProcess; ++frame) {
        for (int32_t ch = 0; ch < channels; ++ch) {
            const float sample = audioData[frame * channels + ch];

            if (sample != 0.0f) {
                allZero = false;
            }

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

    // Update silence detection counters
    if (allZero) {
        consecutiveZeroCallbacks_.fetch_add(1, std::memory_order_relaxed);
    } else {
        consecutiveZeroCallbacks_.store(0, std::memory_order_relaxed);
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
    resetSilenceCounters();
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

size_t PureDataInputSource::calculateOptimalBufferSize(int32_t sampleRate, int32_t channelCount) const {
    LOGD("Calculating optimal buffer size for sampleRate=%d, channels=%d", sampleRate, channelCount);

    // Get device information for adaptive sizing
    char device_brand[PROP_VALUE_MAX];
    char device_model[PROP_VALUE_MAX];
    char hardware[PROP_VALUE_MAX];

    __system_property_get("ro.product.brand", device_brand);
    __system_property_get("ro.product.model", device_model);
    __system_property_get("ro.hardware", hardware);

    // Get number of CPU cores
    const long num_cores = sysconf(_SC_NPROCESSORS_ONLN);

    LOGD("Device info: brand=%s, model=%s, hardware=%s, cores=%ld",
         device_brand, device_model, hardware, num_cores);

    size_t bufferSize = DEFAULT_RING_BUFFER_SIZE;

    // Base size calculation on sample rate
    if (sampleRate >= 96000) {
        // High sample rates need larger buffers
        bufferSize = std::max(bufferSize, static_cast<size_t>(16384));
    } else if (sampleRate <= 22050) {
        // Lower sample rates can use smaller buffers
        bufferSize = std::min(bufferSize, static_cast<size_t>(4096));
    }

    // Adjust based on channel count
    if (channelCount > 2) {
        // Multi-channel needs more buffering
        bufferSize = std::min(bufferSize * 2, MAX_RING_BUFFER_SIZE);
    }

    // Device-specific optimizations based on common characteristics
    // Lower-end devices get smaller buffers for memory efficiency
    if (num_cores <= 4) {
        // Likely older or lower-end device
        bufferSize = std::min(bufferSize, static_cast<size_t>(4096));
        LOGD("Applied low-core optimization: reduced buffer size");
    } else if (num_cores >= 8) {
        // High-end device can handle larger buffers
        bufferSize = std::max(bufferSize, static_cast<size_t>(8192));
        LOGD("Applied high-core optimization: increased buffer size");
    }

    // Brand-specific conservative optimizations for known problematic devices
    if (strstr(device_brand, "samsung") != nullptr && strstr(device_model, "Galaxy A") != nullptr) {
        // Samsung Galaxy A series are often budget devices
        bufferSize = std::min(bufferSize, static_cast<size_t>(4096));
        LOGD("Applied Samsung Galaxy A optimization: conservative buffer size");
    } else if (strstr(hardware, "mt") != nullptr || strstr(hardware, "mediatek") != nullptr) {
        // MediaTek processors often need conservative settings
        bufferSize = std::min(bufferSize, static_cast<size_t>(4096));
        LOGD("Applied MediaTek optimization: conservative buffer size");
    }

    // Ensure buffer size is within bounds and is a power of 2
    bufferSize = std::max(MIN_RING_BUFFER_SIZE, std::min(MAX_RING_BUFFER_SIZE, bufferSize));

    // Round to nearest power of 2 for efficient ring buffer operation
    size_t powerOfTwo = 1;
    while (powerOfTwo < bufferSize) {
        powerOfTwo <<= 1;
    }

    // If we went over, check which is closer
    if (powerOfTwo > bufferSize && (powerOfTwo - bufferSize) > (bufferSize - (powerOfTwo >> 1))) {
        powerOfTwo >>= 1;
    }

    bufferSize = std::max(MIN_RING_BUFFER_SIZE, std::min(MAX_RING_BUFFER_SIZE, powerOfTwo));

    LOGD("Calculated optimal buffer size: %zu", bufferSize);
    return bufferSize;
}

bool PureDataInputSource::isDigitalSilence(uint64_t graceCallbacks, uint64_t silenceCallbacks) const {
    const uint64_t total = totalInputCallbacks_.load(std::memory_order_acquire);
    const uint64_t consecutive = consecutiveZeroCallbacks_.load(std::memory_order_acquire);
    return total > graceCallbacks && consecutive >= silenceCallbacks;
}

void PureDataInputSource::resetSilenceCounters() {
    consecutiveZeroCallbacks_.store(0, std::memory_order_relaxed);
    totalInputCallbacks_.store(0, std::memory_order_relaxed);
}
