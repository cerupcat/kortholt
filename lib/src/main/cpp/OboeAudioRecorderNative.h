#ifndef OBOEAUDIORECORDERNATIVE_H
#define OBOEAUDIORECORDERNATIVE_H

#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <fstream>
#include "AudioRecorderCallback.h"
#include "LockFreeRingBuffer.h"
#include "WaveFileWriter.h"

/**
 * Native audio recorder that captures PCM data from PureDataInputSource
 * and writes it to a WAV file.
 *
 * Architecture:
 * - Audio callback (real-time) writes to lock-free ring buffer
 * - Background writer thread reads from buffer and writes to file
 * - Uses existing WaveFileWriter for WAV format handling
 *
 * This ensures the audio callback remains fast and lock-free while
 * file I/O happens on a separate thread.
 */
class OboeAudioRecorderNative : public AudioRecorderCallback {
public:
    OboeAudioRecorderNative();
    ~OboeAudioRecorderNative() override;

    // Non-copyable, non-movable
    OboeAudioRecorderNative(const OboeAudioRecorderNative&) = delete;
    OboeAudioRecorderNative& operator=(const OboeAudioRecorderNative&) = delete;
    OboeAudioRecorderNative(OboeAudioRecorderNative&&) = delete;
    OboeAudioRecorderNative& operator=(OboeAudioRecorderNative&&) = delete;

    /**
     * Start recording to a WAV file.
     *
     * @param filePath Output WAV file path
     * @param sampleRate Sample rate in Hz (e.g., 44100)
     * @param channelCount Number of channels (1=mono, 2=stereo)
     * @param bitsPerSample Bits per sample (16 or 24)
     * @return true if recording started successfully
     */
    bool startRecording(const std::string& filePath,
                       int32_t sampleRate,
                       int32_t channelCount,
                       int32_t bitsPerSample = 16);

    /**
     * Stop recording and finalize the WAV file.
     * Blocks until all buffered audio is written and file is closed.
     *
     * @return true if stopped successfully
     */
    bool stopRecording();

    /**
     * Pause recording (stop writing data but keep file open).
     * @return true if paused successfully
     */
    bool pauseRecording();

    /**
     * Resume recording after pause.
     * @return true if resumed successfully
     */
    bool resumeRecording();

    /**
     * Check if currently recording (not paused).
     */
    bool isRecording() const {
        return state_.load(std::memory_order_acquire) == State::RECORDING;
    }

    /**
     * Check if paused.
     */
    bool isPaused() const {
        return state_.load(std::memory_order_acquire) == State::PAUSED;
    }

    /**
     * AudioRecorderCallback implementation - receives audio from PureDataInputSource.
     * Called on real-time audio thread - MUST be lock-free!
     */
    void onAudioData(const float* audioData, int32_t numFrames, int32_t channelCount) override;

private:
    enum class State {
        IDLE,
        RECORDING,
        PAUSED,
        STOPPING
    };

    /**
     * Background thread function that reads from ring buffer and writes to file.
     */
    void writerThreadFunction();

    /**
     * Flush any remaining data from ring buffer to file.
     */
    void flushRemainingData();

    // State
    std::atomic<State> state_{State::IDLE};

    // Audio format
    int32_t sampleRate_{0};
    int32_t channelCount_{0};
    int32_t bitsPerSample_{16};

    // Ring buffer for lock-free audio data passing
    static constexpr size_t RING_BUFFER_SIZE = 8192;  // ~185ms @ 44.1kHz stereo
    std::unique_ptr<AudioRingBuffer> ringBuffer_;

    // Writer thread
    std::unique_ptr<std::thread> writerThread_;

    // WAV file output stream
    class FileOutputStream : public WaveFileOutputStream {
    public:
        explicit FileOutputStream(const std::string& filePath);
        ~FileOutputStream() override;

        void write(uint8_t b) override;
        bool isOpen() const { return file_.is_open(); }
        void close();

    private:
        std::ofstream file_;
    };

    std::unique_ptr<FileOutputStream> outputStream_;
    std::unique_ptr<WaveFileWriter> waveWriter_;

    // Statistics
    std::atomic<uint64_t> totalFramesWritten_{0};
    std::atomic<uint64_t> droppedFrames_{0};
};

#endif // OBOEAUDIORECORDERNATIVE_H
