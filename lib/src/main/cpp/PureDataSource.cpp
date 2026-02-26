#include "PureDataSource.h"
#include <android/log.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <sys/system_properties.h>
#include <unistd.h>

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
    tempBufferSize_(0),
    useConservativeSettings_(false),
    adaptiveTicksPerBuffer_(ticksPerBuffer) {
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
    return adaptiveTicksPerBuffer_ * libpd_blocksize();
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

    // Apply device-specific tuning before initialization
    applyDeviceSpecificTuning(sampleRate, channelCount);

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
        // Zero output if not initialized - always provide silence rather than random data
        if (audioData && numFrames > 0) {
            const int32_t outputChans = outputChannels_.load(std::memory_order_acquire);
            std::memset(audioData, 0, numFrames * outputChans * sizeof(float));
        }
        return;
    }

    totalCallbacks_.fetch_add(1, std::memory_order_relaxed);

    // Robust frame count validation with graceful degradation
    if (static_cast<size_t>(numFrames) > maxFramesPerCallback_) {
        numFrames = static_cast<int32_t>(maxFramesPerCallback_);
        failedCallbacks_.fetch_add(1, std::memory_order_relaxed);
    }

    // Validate frame count is aligned to Pure Data block size for stability
    const int32_t blockSize = libpd_blocksize();
    const int32_t alignedFrames = (numFrames / blockSize) * blockSize;

    if (alignedFrames != numFrames) {
        numFrames = alignedFrames;
        failedCallbacks_.fetch_add(1, std::memory_order_relaxed);
        if (numFrames <= 0) {
            // If we can't process any complete blocks, output silence
            const int32_t outputChans = outputChannels_.load(std::memory_order_acquire);
            std::memset(audioData, 0, numFrames * outputChans * sizeof(float));
            return;
        }
    }

    // Attempt processing with error recovery
    bool processingSucceeded = false;
    try {
        processingSucceeded = processPdTicks(numFrames, audioData);
    } catch (...) {
        // Catch any exceptions to prevent audio thread crashes
        processingSucceeded = false;
    }

    if (!processingSucceeded) {
        failedCallbacks_.fetch_add(1, std::memory_order_relaxed);
        // Always output silence on failure rather than leaving uninitialized data
        const int32_t outputChans = outputChannels_.load(std::memory_order_acquire);
        std::memset(audioData, 0, numFrames * outputChans * sizeof(float));

        // Log periodic warnings to avoid log spam
        const uint64_t totalCalls = totalCallbacks_.load(std::memory_order_relaxed);
        if (totalCalls % 1000 == 0) {  // Log every 1000 calls
            const uint64_t failures = failedCallbacks_.load(std::memory_order_relaxed);
            const uint64_t nonFinite = nonFiniteOutputs_.load(std::memory_order_relaxed);
            LOGW("Audio stats: %llu failures, %llu non-finite out of %llu calls",
                 static_cast<unsigned long long>(failures),
                 static_cast<unsigned long long>(nonFinite),
                 static_cast<unsigned long long>(totalCalls));
        }
    }
}

bool PureDataSource::processPdTicks(int32_t numFrames, float *outputData) {
    // Validate parameters before processing
    if (!outputData || numFrames <= 0) {
        return false;
    }

    const int32_t inputChans = inputChannels_.load(std::memory_order_acquire);
    const int32_t outputChans = outputChannels_.load(std::memory_order_acquire);

    // Calculate number of Pure Data ticks
    const int32_t blockSize = libpd_blocksize();
    if (blockSize <= 0) {
        LOGE("Invalid Pure Data block size: %d", blockSize);
        return false;
    }

    const int32_t ticks = numFrames / blockSize;
    if (ticks <= 0) {
        // Not enough frames for a complete block, just return silence
        std::memset(outputData, 0, numFrames * outputChans * sizeof(float));
        return true;
    }

    // Prepare input buffer for Pure Data with additional safety checks
    float* pdInputBuffer = nullptr;
    if (inputChans > 0 && inputSource_ && inputBuffer_) {
        pdInputBuffer = inputBuffer_.get();

        // Safely get input audio from the input source
        try {
            const size_t framesRead = inputSource_->getInputAudio(pdInputBuffer, numFrames);

            // Zero-fill any remaining frames if we didn't get enough input
            if (framesRead < static_cast<size_t>(numFrames)) {
                const size_t remainingFrames = numFrames - framesRead;
                const size_t remainingBytes = remainingFrames * inputChans * sizeof(float);
                std::memset(&pdInputBuffer[framesRead * inputChans], 0, remainingBytes);
            }

            // Validate input data for NaN/infinity to prevent Pure Data issues
            for (size_t i = 0; i < numFrames * inputChans; ++i) {
                if (!std::isfinite(pdInputBuffer[i])) {
                    pdInputBuffer[i] = 0.0f;  // Replace non-finite values with silence
                }
            }
        } catch (...) {
            // If input processing fails, use silence
            std::memset(pdInputBuffer, 0, numFrames * inputChans * sizeof(float));
        }
    }

    // Process with Pure Data with additional safety
    try {
        // Pre-initialize output buffer to prevent garbage output on PD failure
        std::memset(outputData, 0, numFrames * outputChans * sizeof(float));

        libpd_process_float(ticks, pdInputBuffer, outputData);

        // Periodically check if PD is producing non-zero output
        const uint64_t callCount = totalCallbacks_.load(std::memory_order_relaxed);
        if (callCount % 5000 == 1) {
            float maxAbs = 0.0f;
            const size_t totalSamples = numFrames * outputChans;
            for (size_t i = 0; i < totalSamples; ++i) {
                float absVal = std::fabs(outputData[i]);
                if (absVal > maxAbs) maxAbs = absVal;
            }
            LOGD("DIAG: callback #%llu, frames=%d, ticks=%d, inputChans=%d, outputChans=%d, "
                 "pdInput=%s, maxAbsSample=%.6f",
                 static_cast<unsigned long long>(callCount), numFrames, ticks,
                 inputChans, outputChans,
                 pdInputBuffer ? "valid" : "NULL", maxAbs);
        }

        // Post-process validation: check for NaN/infinity in output
        bool outputValid = true;
        for (size_t i = 0; i < numFrames * outputChans; ++i) {
            if (!std::isfinite(outputData[i])) {
                outputData[i] = 0.0f;  // Replace non-finite values with silence
                outputValid = false;
            }
        }

        if (!outputValid) {
            nonFiniteOutputs_.fetch_add(1, std::memory_order_relaxed);
        }

        return true;
    } catch (...) {
        // If Pure Data processing fails, ensure we output silence
        std::memset(outputData, 0, numFrames * outputChans * sizeof(float));
        return false;
    }
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

    const uint64_t nonFinite = nonFiniteOutputs_.load(std::memory_order_acquire);

    Statistics stats;
    stats.totalCallbacks = total;
    stats.failedCallbacks = failed;
    stats.nonFiniteOutputs = nonFinite;
    stats.failurePercentage = (total > 0) ? (static_cast<double>(failed) / total * 100.0) : 0.0;

    return stats;
}

void PureDataSource::resetStatistics() {
    totalCallbacks_.store(0, std::memory_order_relaxed);
    failedCallbacks_.store(0, std::memory_order_relaxed);
    nonFiniteOutputs_.store(0, std::memory_order_relaxed);
}

void PureDataSource::applyDeviceSpecificTuning(int32_t sampleRate, int32_t channelCount) {
    LOGD("Applying device-specific tuning for sampleRate=%d, channels=%d", sampleRate, channelCount);

    // Get device information for adaptive tuning
    char device_brand[PROP_VALUE_MAX];
    char device_model[PROP_VALUE_MAX];
    char hardware[PROP_VALUE_MAX];
    char sdk_version[PROP_VALUE_MAX];

    __system_property_get("ro.product.brand", device_brand);
    __system_property_get("ro.product.model", device_model);
    __system_property_get("ro.hardware", hardware);
    __system_property_get("ro.build.version.sdk", sdk_version);

    // Get number of CPU cores
    const long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    const int sdk_int = atoi(sdk_version);

    LOGD("Device info: brand=%s, model=%s, hardware=%s, SDK=%d, cores=%ld",
         device_brand, device_model, hardware, sdk_int, num_cores);

    // Start with default settings
    adaptiveTicksPerBuffer_ = ticksPerBuffer_;
    useConservativeSettings_ = false;

    // Apply conservative settings for older Android versions
    if (sdk_int < 23) {  // Android 6.0 (API 23) and below
        useConservativeSettings_ = true;
        adaptiveTicksPerBuffer_ = std::max(8, ticksPerBuffer_);
        LOGD("Applied old Android version optimization: increased buffer safety");
    }

    // Apply CPU core-based optimizations
    if (num_cores <= 4) {
        // Low-end devices: prioritize stability over latency
        useConservativeSettings_ = true;
        adaptiveTicksPerBuffer_ = std::max(8, adaptiveTicksPerBuffer_);
        LOGD("Applied low-core optimization: conservative settings for %ld cores", num_cores);
    } else if (num_cores >= 8) {
        // High-end devices: can handle more aggressive settings
        adaptiveTicksPerBuffer_ = std::min(4, adaptiveTicksPerBuffer_);
        LOGD("Applied high-core optimization: optimized settings for %ld cores", num_cores);
    }

    // Sample rate specific adjustments
    if (sampleRate >= 96000) {
        // High sample rates need larger buffers for stability
        adaptiveTicksPerBuffer_ = std::max(8, adaptiveTicksPerBuffer_);
        LOGD("Applied high sample rate optimization: increased buffer for %d Hz", sampleRate);
    } else if (sampleRate <= 22050) {
        // Lower sample rates can use smaller buffers
        adaptiveTicksPerBuffer_ = std::max(2, std::min(4, adaptiveTicksPerBuffer_));
        LOGD("Applied low sample rate optimization: optimized buffer for %d Hz", sampleRate);
    }

    // Channel count adjustments
    if (channelCount > 2) {
        // Multi-channel processing needs larger buffers
        adaptiveTicksPerBuffer_ = std::max(6, adaptiveTicksPerBuffer_);
        LOGD("Applied multi-channel optimization: increased buffer for %d channels", channelCount);
    }

    // Device-specific known issues and optimizations
    if (strstr(device_brand, "samsung") != nullptr) {
        if (strstr(device_model, "Galaxy A") != nullptr || strstr(device_model, "Galaxy J") != nullptr) {
            // Samsung budget devices often have audio processing issues
            useConservativeSettings_ = true;
            adaptiveTicksPerBuffer_ = std::max(12, adaptiveTicksPerBuffer_);
            LOGD("Applied Samsung budget device optimization: very conservative settings");
        } else if (strstr(device_model, "Galaxy S") != nullptr && strstr(device_model, "Galaxy S1") == nullptr) {
            // Samsung flagship devices (but not S10/S1x which might match S1)
            adaptiveTicksPerBuffer_ = std::max(4, std::min(8, adaptiveTicksPerBuffer_));
            LOGD("Applied Samsung flagship optimization: balanced settings");
        }
    }

    if (strstr(hardware, "mt") != nullptr || strstr(hardware, "mediatek") != nullptr) {
        // MediaTek processors often have inconsistent audio performance
        useConservativeSettings_ = true;
        adaptiveTicksPerBuffer_ = std::max(10, adaptiveTicksPerBuffer_);
        LOGD("Applied MediaTek optimization: conservative settings for stability");
    }

    if (strstr(hardware, "msm") != nullptr || strstr(hardware, "qcom") != nullptr || strstr(hardware, "sdm") != nullptr) {
        // Qualcomm Snapdragon processors generally have good audio performance
        if (num_cores >= 8) {
            adaptiveTicksPerBuffer_ = std::max(2, std::min(6, adaptiveTicksPerBuffer_));
            LOGD("Applied Qualcomm high-end optimization: aggressive settings");
        } else {
            adaptiveTicksPerBuffer_ = std::max(4, std::min(8, adaptiveTicksPerBuffer_));
            LOGD("Applied Qualcomm mid-range optimization: balanced settings");
        }
    }

    if (strstr(device_brand, "huawei") != nullptr || strstr(device_brand, "honor") != nullptr) {
        // Huawei/Honor devices have varying audio performance
        useConservativeSettings_ = true;
        adaptiveTicksPerBuffer_ = std::max(8, adaptiveTicksPerBuffer_);
        LOGD("Applied Huawei/Honor optimization: conservative settings");
    }

    if (strstr(device_brand, "xiaomi") != nullptr || strstr(device_brand, "redmi") != nullptr) {
        // Xiaomi devices generally have good audio performance but vary widely
        adaptiveTicksPerBuffer_ = std::max(6, std::min(10, adaptiveTicksPerBuffer_));
        LOGD("Applied Xiaomi optimization: moderate settings");
    }

    if (strstr(device_brand, "oppo") != nullptr || strstr(device_brand, "oneplus") != nullptr || strstr(device_brand, "vivo") != nullptr) {
        // BBK Electronics family devices (OnePlus usually performs better)
        if (strstr(device_brand, "oneplus") != nullptr) {
            adaptiveTicksPerBuffer_ = std::max(4, std::min(8, adaptiveTicksPerBuffer_));
            LOGD("Applied OnePlus optimization: balanced settings");
        } else {
            adaptiveTicksPerBuffer_ = std::max(8, adaptiveTicksPerBuffer_);
            LOGD("Applied OPPO/Vivo optimization: conservative settings");
        }
    }

    // Ensure ticks per buffer is reasonable
    adaptiveTicksPerBuffer_ = std::max(1, std::min(32, adaptiveTicksPerBuffer_));

    // Log final settings
    if (adaptiveTicksPerBuffer_ != ticksPerBuffer_) {
        LOGD("Device tuning complete: adjusted ticksPerBuffer from %d to %d (conservative=%s)",
             ticksPerBuffer_, adaptiveTicksPerBuffer_, useConservativeSettings_ ? "yes" : "no");
    } else {
        LOGD("Device tuning complete: using default ticksPerBuffer=%d (conservative=%s)",
             adaptiveTicksPerBuffer_, useConservativeSettings_ ? "yes" : "no");
    }
}