#ifndef KORTHOLT_H
#define KORTHOLT_H

#include <atomic>
#include <oboe/Oboe.h>
#include <PdBase.hpp>
#include <IRestartable.h>
#include <DefaultErrorCallback.h>
#include <LatencyTuningCallback.h>
#include "PureDataSource.h"
#include "PureDataInputSource.h"

class Kortholt : public IRestartable {

public:
    /**
     * Create Kortholt audio engine.
     * @param cpuIds CPU cores for thread affinity
     * @param stream true for real-time streaming, false for file output
     * @param inputDeviceId Oboe device ID for input (-1 for system default)
     * @param outputDeviceId Oboe device ID for output (-1 for system default)
     */
    Kortholt(std::vector<int> cpuIds, bool stream,
             int32_t inputDeviceId = oboe::kUnspecified,
             int32_t outputDeviceId = oboe::kUnspecified);

    virtual ~Kortholt();

    virtual void restart() override;

    /**
     * Set audio device IDs and restart streams.
     * Use oboe::kUnspecified (-1) for system default device.
     * @param inputDeviceId Oboe device ID for input
     * @param outputDeviceId Oboe device ID for output
     */
    void setDeviceIds(int32_t inputDeviceId, int32_t outputDeviceId);

    int32_t saveWaveFile(
            const char *fileName,
            const int32_t duration,
            const char *startBang,
            const char *stopBang
    );

    // Message sending functions to Pure Data
    void sendFloat(const char *dest, float value);
    void sendBang(const char *dest);
    void sendSymbol(const char *dest, const char *symbol);

    // Patch management functions
    bool openPatch(const char *patch, const char *path);
    void addToSearchPath(const char *path);

    // Performance monitoring functions
    void logPerformanceStatistics();

    /**
     * Start output and input streams. Call after patch is opened to avoid
     * race between libpd_process_float and libpd_openfile.
     */
    void startStreams();

    /**
     * Create and start the input (microphone) stream.
     * Call when RECORD_AUDIO permission is granted.
     * Safe to call while output stream is already running since it only
     * writes to a lock-free ring buffer read by PureDataSource.
     */
    void enableMicInput();

    // Audio recorder integration
    void setRecorderCallback(class AudioRecorderCallback *callback);
    void clearRecorderCallback();

    // Reverb effect control for recording
    void setReverbEnabled(bool enabled);
    void setReverbLevel(float level);

    /**
     * Check if the input stream is producing digital silence.
     * Delegates to PureDataInputSource with the configured thresholds.
     */
    bool isInputDigitalSilence();

    /**
     * Close and reopen the input stream with a different InputPreset and/or AudioApi.
     * The output stream, PD patch, and all state remain untouched.
     * Resets silence detection counters after reopening.
     *
     * @param preset The InputPreset to use (e.g., Generic, VoiceCommunication)
     * @param audioApi The AudioApi to use (Unspecified lets Oboe choose, OpenSLES forces legacy)
     */
    void reopenInputStream(oboe::InputPreset preset,
                           oboe::AudioApi audioApi = oboe::AudioApi::Unspecified);

    /**
     * Reset silence detection counters without reopening the stream.
     */
    void resetInputSilenceDetection();

    /**
     * Get the sample rate of the output stream.
     * This is the rate at which PureData and audio recording operate.
     * Returns 0 if the output stream is not initialized.
     */
    int32_t getStreamSampleRate() const;

private:
    // ─── DESTRUCTION ORDER CRITICAL ───────────────────────────────────
    // C++ destroys members in REVERSE declaration order.
    // errorCallback and streamLock are declared FIRST so they are
    // destroyed LAST.  This guarantees that Oboe error callback threads
    // (which may fire during stream destruction) can still safely access
    // mMutex inside DefaultErrorCallback and, if they slip past the
    // atomic-disabled check, streamLock inside Kortholt.
    // ──────────────────────────────────────────────────────────────────
    std::shared_ptr<DefaultErrorCallback> errorCallback;
    mutable std::mutex streamLock;

    bool isStream;
    bool mInputEnabled = false;
    std::shared_ptr<oboe::AudioStream> outputStream;  // For tone generation
    std::shared_ptr<oboe::AudioStream> inputStream;   // For tuner microphone input
    std::shared_ptr<PureDataSource> pureDataSource;
    std::shared_ptr<PureDataInputSource> pureDataInputSource;
    std::shared_ptr<LatencyTuningCallback> outputCallback;
    std::shared_ptr<LatencyTuningCallback> inputCallback;
    int32_t ticksPerBuffer;
    int32_t bufferSize;

    // Device selection (oboe::kUnspecified for system default)
    int32_t mInputDeviceId;
    int32_t mOutputDeviceId;

    // Input stream configuration for silence fallback
    // Reset to defaults on enableMicInput() and restart() (new device = fresh chain)
    oboe::InputPreset mInputPreset = oboe::InputPreset::VoiceRecognition;
    oboe::AudioApi mInputAudioApi = oboe::AudioApi::Unspecified;

    // Debounce guard: prevents double restart when both streams disconnect simultaneously
    std::atomic<bool> mRestarting{false};

    // Workaround for Oboe bug google/oboe#2325:
    // FilterAudioStream use-after-free when shared_ptr is released before
    // Oboe's error callback thread finishes. Old streams are stashed here
    // to keep FilterAudioStream alive until the next restart/destruction.
    std::vector<std::shared_ptr<oboe::AudioStream>> mRetiredStreams;

    oboe::Result createPlaybackStream();
    oboe::Result createRecordingStream();
    bool openAndStartInputStream(const char *label);
    void stopAndCloseStream(std::shared_ptr<oboe::AudioStream> &stream,
                            const char *label);

    void start();
    void stop();

    static int32_t calculateTicksPerBuffer();

    // Silence detection constants (configurable for tuning)
    static constexpr uint64_t SILENCE_GRACE_CALLBACKS = 62;     // ~500ms at 48kHz/384-frame buffers
    static constexpr uint64_t SILENCE_DETECTION_CALLBACKS = 62;  // ~500ms of consecutive zeros
};

#endif //KORTHOLT_H
