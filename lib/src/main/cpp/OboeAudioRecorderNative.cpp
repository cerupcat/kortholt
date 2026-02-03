#include "OboeAudioRecorderNative.h"
#include <android/log.h>
#include <cstring>
#include <chrono>
#include <algorithm>

#define LOG_TAG "OboeAudioRecorderNative"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {
    // Convert float samples in range [-1.0, 1.0] to int16 PCM format.
    // Clamps values to prevent overflow during conversion.
    inline int16_t floatToInt16(float sample) {
        sample = std::clamp(sample, -1.0f, 1.0f);
        return static_cast<int16_t>(sample * 32767.0f);
    }

    // Convert a buffer of float samples to int16 in place.
    void convertFloatToInt16(const float* source, int16_t* dest, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            dest[i] = floatToInt16(source[i]);
        }
    }
}

// OboeAudioRecorderNative implementation
OboeAudioRecorderNative::OboeAudioRecorderNative() {
    LOGD("OboeAudioRecorderNative created");
}

OboeAudioRecorderNative::~OboeAudioRecorderNative() {
    if (state_.load() != State::IDLE) {
        stopRecording();
    }
    LOGD("OboeAudioRecorderNative destroyed");
}

bool OboeAudioRecorderNative::startRecording(const std::string& filePath,
                                             int32_t sampleRate,
                                             int32_t channelCount,
                                             int32_t bitsPerSample) {
    State expected = State::IDLE;
    if (!state_.compare_exchange_strong(expected, State::RECORDING)) {
        LOGE("Cannot start recording - already recording or paused");
        return false;
    }

    LOGI("Starting recording: %s (sr=%d, ch=%d, bits=%d)",
         filePath.c_str(), sampleRate, channelCount, bitsPerSample);

    // Validate parameters
    const bool isValidParams = sampleRate >= 8000 && sampleRate <= 192000 &&
                               channelCount >= 1 && channelCount <= 2 &&
                               (bitsPerSample == 16 || bitsPerSample == 24);

    if (!isValidParams) {
        LOGE("Invalid parameters: sr=%d, ch=%d, bits=%d", sampleRate, channelCount, bitsPerSample);
        state_.store(State::IDLE);
        return false;
    }

    // Store audio format
    sampleRate_ = sampleRate;
    channelCount_ = channelCount;
    bitsPerSample_ = bitsPerSample;

    // Create ring buffer
    ringBuffer_ = std::make_unique<AudioRingBuffer>(RING_BUFFER_SIZE);

    // Initialize dr_wav for writing
    drwav_data_format format;
    format.container = drwav_container_riff;
    format.channels = channelCount;
    format.sampleRate = sampleRate;

    // Match format to bitsPerSample requested by Kotlin
    // Note: dr_wav handles both 16 and 24-bit PCM with DR_WAVE_FORMAT_PCM
    format.format = DR_WAVE_FORMAT_PCM;
    format.bitsPerSample = bitsPerSample;

    if (!drwav_init_file_write(&wav_, filePath.c_str(), &format, nullptr)) {
        LOGE("Failed to initialize dr_wav for file: %s", filePath.c_str());
        state_.store(State::IDLE);
        return false;
    }

    wavInitialized_ = true;
    LOGD("dr_wav initialized successfully");

    // Reset statistics
    totalFramesWritten_.store(0, std::memory_order_release);
    droppedFrames_.store(0, std::memory_order_release);

    // Start writer thread
    writerThread_ = std::make_unique<std::thread>(
        &OboeAudioRecorderNative::writerThreadFunction, this);

    LOGI("Recording started successfully");
    return true;
}

bool OboeAudioRecorderNative::stopRecording() {
    State currentState = state_.load();
    if (currentState == State::IDLE) {
        LOGD("Not recording - nothing to stop");
        return true;
    }

    LOGI("Stopping recording...");

    // Signal writer thread to stop
    state_.store(State::STOPPING, std::memory_order_release);

    // Wait for writer thread to finish
    if (writerThread_ && writerThread_->joinable()) {
        writerThread_->join();
    }

    // Cleanup
    if (wavInitialized_) {
        drwav_uninit(&wav_);  // Automatically finalizes WAV header!
        wavInitialized_ = false;
        LOGD("dr_wav uninitialized and WAV file finalized");
    }
    ringBuffer_.reset();
    writerThread_.reset();

    state_.store(State::IDLE, std::memory_order_release);

    const uint64_t framesWritten = totalFramesWritten_.load();
    const uint64_t framesDropped = droppedFrames_.load();

    LOGI("Recording stopped. Frames written: %llu, dropped: %llu",
         static_cast<unsigned long long>(framesWritten),
         static_cast<unsigned long long>(framesDropped));

    return true;
}

bool OboeAudioRecorderNative::pauseRecording() {
    State expected = State::RECORDING;
    if (!state_.compare_exchange_strong(expected, State::PAUSED)) {
        LOGE("Cannot pause - not currently recording");
        return false;
    }

    LOGD("Recording paused");
    return true;
}

bool OboeAudioRecorderNative::resumeRecording() {
    State expected = State::PAUSED;
    if (!state_.compare_exchange_strong(expected, State::RECORDING)) {
        LOGE("Cannot resume - not currently paused");
        return false;
    }

    LOGD("Recording resumed");
    return true;
}

void OboeAudioRecorderNative::onAudioData(const float* audioData,
                                         int32_t numFrames,
                                         int32_t channelCount) {
    // This runs on the real-time audio thread - MUST be fast and lock-free!

    const State currentState = state_.load(std::memory_order_acquire);

    // Only write data if we're actively recording (not paused or stopping)
    if (currentState != State::RECORDING) {
        return;
    }

    // Verify channel count matches
    if (channelCount != channelCount_) {
        // Log once, but don't spam
        static std::atomic<bool> errorLogged{false};
        if (!errorLogged.exchange(true)) {
            LOGE("Channel count mismatch: expected %d, got %d", channelCount_, channelCount);
        }
        return;
    }

    if (!ringBuffer_) {
        return;
    }

    // Write to ring buffer (lock-free)
    const int32_t totalSamples = numFrames * channelCount;
    const size_t written = ringBuffer_->write(audioData, totalSamples);

    // Track dropped frames
    if (written < static_cast<size_t>(totalSamples)) {
        const size_t droppedSamples = totalSamples - written;
        const size_t droppedFramesCount = droppedSamples / channelCount;
        droppedFrames_.fetch_add(droppedFramesCount, std::memory_order_relaxed);
    }
}

void OboeAudioRecorderNative::writerThreadFunction() {
    LOGD("Writer thread started");

    // Allocate working buffer
    const size_t READ_CHUNK_SIZE = 512;
    auto floatBuffer = std::make_unique<float[]>(READ_CHUNK_SIZE);
    auto int16Buffer = std::make_unique<int16_t[]>(READ_CHUNK_SIZE);

    while (state_.load(std::memory_order_acquire) == State::RECORDING ||
           state_.load(std::memory_order_acquire) == State::PAUSED) {

        // Check if we should be writing (not paused)
        if (state_.load(std::memory_order_acquire) == State::PAUSED) {
            // Paused - sleep and continue
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Read from ring buffer
        const size_t available = ringBuffer_->availableForRead();
        const size_t toRead = std::min(available, READ_CHUNK_SIZE);

        if (toRead > 0) {
            const size_t read = ringBuffer_->read(floatBuffer.get(), toRead);

            if (read > 0 && wavInitialized_) {
                // Convert float to int16 for PCM format
                convertFloatToInt16(floatBuffer.get(), int16Buffer.get(), read);

                // Write to WAV file using dr_wav
                const size_t numFrames = read / channelCount_;
                const drwav_uint64 framesWritten = drwav_write_pcm_frames(&wav_, numFrames, int16Buffer.get());

                // Update statistics
                totalFramesWritten_.fetch_add(framesWritten, std::memory_order_relaxed);

                if (framesWritten != numFrames) {
                    LOGE("Warning: Expected to write %zu frames but wrote %llu",
                         numFrames, static_cast<unsigned long long>(framesWritten));
                }
            }
        } else {
            // No data available - sleep briefly to avoid spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Flush remaining data when stopping
    LOGD("Flushing remaining data...");
    flushRemainingData();

    LOGD("Writer thread finished");
}

void OboeAudioRecorderNative::flushRemainingData() {
    if (!ringBuffer_ || !wavInitialized_) {
        return;
    }

    const size_t BUFFER_SIZE = 1024;
    auto floatBuffer = std::make_unique<float[]>(BUFFER_SIZE);
    auto int16Buffer = std::make_unique<int16_t[]>(BUFFER_SIZE);

    // Read and write all remaining data
    size_t totalFlushed = 0;
    size_t available;
    while ((available = ringBuffer_->availableForRead()) > 0) {
        const size_t toRead = std::min(available, BUFFER_SIZE);
        const size_t read = ringBuffer_->read(floatBuffer.get(), toRead);

        if (read == 0) break;

        // Convert float to int16 for PCM format
        convertFloatToInt16(floatBuffer.get(), int16Buffer.get(), read);

        const size_t numFrames = read / channelCount_;
        drwav_write_pcm_frames(&wav_, numFrames, int16Buffer.get());
        totalFlushed += read;
    }

    LOGD("Flushed %zu samples", totalFlushed);
}
