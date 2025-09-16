#include "PureDataSource.h"
#include <android/log.h>
#include <cstring>
#include <algorithm>
#include <cmath>

#define LOG_TAG "PureDataSource"
#ifndef LOGD
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGW
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#endif

extern "C" void externals_setup(void);

PureDataSource::PureDataSource(int32_t ticksPerBuffer) :
    ticksPerBuffer_(ticksPerBuffer),
    maxFramesPerCallback_(0),
    inputBufferSize_(0),
    tempBufferSize_(0) {
    LOGD("PureDataSource created with ticksPerBuffer=%d", ticksPerBuffer);
}

bool PureDataSource::validateParameters(int32_t sampleRate, int32_t channelCount) const {
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

size_t PureDataSource::calculateMaxFramesPerCallback() const {
    return ticksPerBuffer_ * libpd_blocksize();
}

bool PureDataSource::initializeBuffers() {
    maxFramesPerCallback_ = calculateMaxFramesPerCallback();

    const int32_t inputChans = inputChannels_.load(std::memory_order_relaxed);
    const int32_t outputChans = outputChannels_.load(std::memory_order_relaxed);

    // Calculate buffer sizes
    inputBufferSize_ = maxFramesPerCallback_ * std::max(inputChans, 1);
    tempBufferSize_ = maxFramesPerCallback_ * std::max(outputChans, 2);

    LOGD("Initializing buffers: maxFrames=%zu, inputChans=%d, outputChans=%d",
         maxFramesPerCallback_, inputChans, outputChans);

    // Allocate input buffer (only if we have input channels)
    if (inputChans > 0) {
        inputBuffer_ = std::make_unique<float[]>(inputBufferSize_);
        std::memset(inputBuffer_.get(), 0, inputBufferSize_ * sizeof(float));
        LOGD("Allocated input buffer: %zu samples", inputBufferSize_);
    }

    // Allocate temp buffer for Pure Data processing
    tempBuffer_ = std::make_unique<float[]>(tempBufferSize_);
    std::memset(tempBuffer_.get(), 0, tempBufferSize_ * sizeof(float));
    LOGD("Allocated temp buffer: %zu samples", tempBufferSize_);

    return true;
}

void PureDataSource::setInputChannels(int32_t channels) {
    if (initialized_.load(std::memory_order_acquire)) {
        LOGW("Cannot set input channels after initialization");
        return;
    }

    if (channels < 0 || channels > static_cast<int32_t>(MAX_CHANNELS)) {
        LOGE("Invalid input channel count: %d", channels);
        return;
    }

    inputChannels_.store(channels, std::memory_order_release);
    LOGD("Input channels set to %d", channels);
}

void PureDataSource::setInputSource(std::shared_ptr<PureDataInputSource> source) {
    if (initialized_.load(std::memory_order_acquire)) {
        LOGW("Cannot set input source after initialization");
        return;
    }

    inputSource_ = source;
    LOGD("Input source set");
}

bool PureDataSource::init(int32_t sampleRate, int32_t channelCount) {
    LOGD("Initializing PureDataSource: sampleRate=%d, channelCount=%d, inputChannels=%d, ticksPerBuffer=%d",
         sampleRate, channelCount, inputChannels_.load(std::memory_order_relaxed), ticksPerBuffer_);

    if (!validateParameters(sampleRate, channelCount)) {
        return false;
    }

    // Store output channel count
    outputChannels_.store(channelCount, std::memory_order_release);

    // Initialize libpd's audio processing system
    const int32_t inputChans = inputChannels_.load(std::memory_order_relaxed);
    LOGD("Initializing libpd audio processing system...");
    int initResult = libpd_init_audio(inputChans, channelCount, sampleRate);
    if (initResult != 0) {
        LOGE("Failed to initialize libpd audio system: %d", initResult);
        return false;
    }
    LOGD("libpd audio system initialized successfully with %d input and %d output channels",
         inputChans, channelCount);

    // Enable DSP computation
    LOGD("Enabling DSP computation...");
    libpd_start_message(1);
    libpd_add_float(1.0f);
    libpd_finish_message("pd", "dsp");
    LOGD("DSP computation enabled");

    // Setup externals
    LOGD("Setting up externals...");
    externals_setup();
    LOGD("Externals setup complete");

    // Initialize buffers
    if (!initializeBuffers()) {
        LOGE("Failed to initialize buffers");
        return false;
    }

    // Reset statistics
    resetStatistics();

    // Mark as initialized (this must be last)
    initialized_.store(true, std::memory_order_release);

    LOGD("PureDataSource initialization complete");
    return true;
}

void PureDataSource::renderAudio(float *audioData, int32_t numFrames) {
    // This method runs in the real-time audio output thread
    // It MUST be lock-free and allocation-free

    if (!initialized_.load(std::memory_order_acquire) || !audioData || numFrames <= 0) {
        // Zero output if not initialized
        if (audioData && numFrames > 0) {
            const int32_t outputChans = outputChannels_.load(std::memory_order_acquire);
            std::memset(audioData, 0, numFrames * outputChans * sizeof(float));
        }
        return;
    }

    totalCallbacks_.fetch_add(1, std::memory_order_relaxed);

    // Validate frame count to prevent buffer overruns
    if (static_cast<size_t>(numFrames) > maxFramesPerCallback_) {
        LOGW("Frame count %d exceeds maximum %zu, clamping", numFrames, maxFramesPerCallback_);
        numFrames = static_cast<int32_t>(maxFramesPerCallback_);
        failedCallbacks_.fetch_add(1, std::memory_order_relaxed);
    }

    if (!processPdTicks(numFrames, audioData)) {
        failedCallbacks_.fetch_add(1, std::memory_order_relaxed);
        // Zero output on failure
        const int32_t outputChans = outputChannels_.load(std::memory_order_acquire);
        std::memset(audioData, 0, numFrames * outputChans * sizeof(float));
    }
}

bool PureDataSource::processPdTicks(int32_t numFrames, float *outputData) {
    const int32_t inputChans = inputChannels_.load(std::memory_order_acquire);

    // Calculate number of Pure Data ticks
    const int32_t blockSize = libpd_blocksize();
    const int32_t ticks = numFrames / blockSize;

    if (ticks <= 0) {
        return false;
    }

    // Prepare input buffer for Pure Data
    float* pdInputBuffer = nullptr;
    if (inputChans > 0 && inputSource_) {
        pdInputBuffer = inputBuffer_.get();

        // Get input audio from the input source
        const size_t framesRead = inputSource_->getInputAudio(pdInputBuffer, numFrames);

        // Zero-fill any remaining frames if we didn't get enough input
        if (framesRead < static_cast<size_t>(numFrames)) {
            const size_t remainingFrames = numFrames - framesRead;
            const size_t remainingBytes = remainingFrames * inputChans * sizeof(float);
            std::memset(&pdInputBuffer[framesRead * inputChans], 0, remainingBytes);
        }
    }

    // Process with Pure Data
    libpd_process_float(ticks, pdInputBuffer, outputData);

    return true;
}

bool PureDataSource::processAudio(float *inputData, float *outputData, int32_t numFrames) {
    // This method is NOT real-time safe and should only be used for offline processing
    if (!initialized_.load(std::memory_order_acquire)) {
        return false;
    }

    const int32_t blockSize = libpd_blocksize();
    const int32_t ticks = numFrames / blockSize;

    if (ticks <= 0) {
        return false;
    }

    // Process with Pure Data (this may use mutex internally - not RT safe)
    libpd_process_float(ticks, inputData, outputData);

    return true;
}

void PureDataSource::sendFloat(const char *dest, float value) {
    if (!dest) return;

    LOGD("Sending float to %s: %f", dest, value);
    libpd_float(dest, value);
}

void PureDataSource::sendBang(const char *dest) {
    if (!dest) return;

    LOGD("Sending bang to %s", dest);
    libpd_bang(dest);
}

void PureDataSource::sendSymbol(const char *dest, const char *symbol) {
    if (!dest || !symbol) return;

    LOGD("Sending symbol to %s: %s", dest, symbol);
    libpd_symbol(dest, symbol);
}

bool PureDataSource::openPatch(const char *patch, const char *path) {
    if (!patch || !path) return false;

    LOGD("Opening patch: %s in path: %s", patch, path);
    void *handle = libpd_openfile(patch, path);
    bool success = (handle != nullptr);
    if (success) {
        LOGD("Successfully opened patch: %s", patch);
    } else {
        LOGE("Failed to open patch: %s", patch);
    }
    return success;
}

void PureDataSource::addToSearchPath(const char *path) {
    if (!path) return;

    LOGD("Adding to search path: %s", path);
    libpd_add_to_search_path(path);
}

PureDataSource::Statistics PureDataSource::getStatistics() const {
    const uint64_t total = totalCallbacks_.load(std::memory_order_acquire);
    const uint64_t failed = failedCallbacks_.load(std::memory_order_acquire);

    Statistics stats;
    stats.totalCallbacks = total;
    stats.failedCallbacks = failed;
    stats.failurePercentage = (total > 0) ? (static_cast<double>(failed) / total * 100.0) : 0.0;

    return stats;
}

void PureDataSource::resetStatistics() {
    totalCallbacks_.store(0, std::memory_order_relaxed);
    failedCallbacks_.store(0, std::memory_order_relaxed);
}