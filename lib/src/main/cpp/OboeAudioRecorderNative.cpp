#include "OboeAudioRecorderNative.h"
#include <android/log.h>
#include <cstring>
#include <chrono>

#define LOG_TAG "OboeAudioRecorderNative"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// FileOutputStream implementation
OboeAudioRecorderNative::FileOutputStream::FileOutputStream(const std::string& filePath) {
    file_.open(filePath, std::ios::binary | std::ios::out);
    if (!file_.is_open()) {
        LOGE("Failed to open file: %s", filePath.c_str());
    } else {
        LOGD("Opened file for writing: %s", filePath.c_str());
    }
}

OboeAudioRecorderNative::FileOutputStream::~FileOutputStream() {
    close();
}

void OboeAudioRecorderNative::FileOutputStream::write(uint8_t b) {
    if (file_.is_open()) {
        file_.put(static_cast<char>(b));
    }
}

void OboeAudioRecorderNative::FileOutputStream::close() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
        LOGD("File closed");
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
    if (sampleRate < 8000 || sampleRate > 192000) {
        LOGE("Invalid sample rate: %d", sampleRate);
        state_.store(State::IDLE);
        return false;
    }

    if (channelCount < 1 || channelCount > 2) {
        LOGE("Invalid channel count: %d", channelCount);
        state_.store(State::IDLE);
        return false;
    }

    if (bitsPerSample != 16 && bitsPerSample != 24) {
        LOGE("Invalid bits per sample: %d (must be 16 or 24)", bitsPerSample);
        state_.store(State::IDLE);
        return false;
    }

    // Store audio format
    sampleRate_ = sampleRate;
    channelCount_ = channelCount;
    bitsPerSample_ = bitsPerSample;

    // Create ring buffer
    ringBuffer_ = std::make_unique<AudioRingBuffer>(RING_BUFFER_SIZE);

    // Create output stream
    outputStream_ = std::make_unique<FileOutputStream>(filePath);
    if (!outputStream_->isOpen()) {
        LOGE("Failed to open output file");
        state_.store(State::IDLE);
        return false;
    }

    // Create WAV writer
    waveWriter_ = std::make_unique<WaveFileWriter>(outputStream_.get());
    waveWriter_->setFrameRate(sampleRate);
    waveWriter_->setSamplesPerFrame(channelCount);
    waveWriter_->setBitsPerSample(bitsPerSample);

    // Note: We don't call setFrameCount() - WaveFileWriter will use INT32_MAX
    // and we'll update the header when we stop recording

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
    waveWriter_.reset();
    outputStream_.reset();
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
    auto audioBuffer = std::make_unique<float[]>(READ_CHUNK_SIZE);

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
            const size_t read = ringBuffer_->read(audioBuffer.get(), toRead);

            if (read > 0 && waveWriter_) {
                // Write to WAV file
                waveWriter_->write(audioBuffer.get(), 0, static_cast<int32_t>(read));

                // Update statistics
                totalFramesWritten_.fetch_add(read / channelCount_, std::memory_order_relaxed);
            }
        } else {
            // No data available - sleep briefly to avoid spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Flush remaining data when stopping
    LOGD("Flushing remaining data...");
    flushRemainingData();

    // Close WAV writer to finalize file
    if (waveWriter_) {
        waveWriter_->close();
    }

    if (outputStream_) {
        outputStream_->close();
    }

    LOGD("Writer thread finished");
}

void OboeAudioRecorderNative::flushRemainingData() {
    if (!ringBuffer_ || !waveWriter_) {
        return;
    }

    const size_t BUFFER_SIZE = 1024;
    auto buffer = std::make_unique<float[]>(BUFFER_SIZE);

    // Read and write all remaining data
    size_t totalFlushed = 0;
    while (true) {
        const size_t available = ringBuffer_->availableForRead();
        if (available == 0) {
            break;
        }

        const size_t toRead = std::min(available, BUFFER_SIZE);
        const size_t read = ringBuffer_->read(buffer.get(), toRead);

        if (read > 0) {
            waveWriter_->write(buffer.get(), 0, static_cast<int32_t>(read));
            totalFlushed += read;
        } else {
            break;
        }
    }

    LOGD("Flushed %zu samples", totalFlushed);
}
