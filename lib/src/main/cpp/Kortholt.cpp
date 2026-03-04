#include <cinttypes>
#include <memory>
#include <cmath>
#include <chrono>
#include <thread>
#include <fstream>
#include <android/log.h>
#include <jni.h>
#include "Kortholt.h"
#include "dr_wav.h"

#define LOG_TAG "Kortholt"
// Use Oboe's existing logging macros to avoid redefinition
#ifndef LOGD
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

const int32_t DEFAULT_TICKS_FOR_STREAM = 8;
const int32_t DEFAULT_TICKS = 16;
const int32_t STREAM_BUFFER_MULTIPLIER = 2;

Kortholt::Kortholt(std::vector<int> cpuIds, bool stream,
                   int32_t inputDeviceId, int32_t outputDeviceId) {
    LOGD("Kortholt constructor: stream=%s, inputDeviceId=%d, outputDeviceId=%d",
         stream ? "true" : "false", inputDeviceId, outputDeviceId);

    isStream = stream;
    mInputDeviceId = inputDeviceId;
    mOutputDeviceId = outputDeviceId;
    ticksPerBuffer = stream ? calculateTicksPerBuffer() : DEFAULT_TICKS;
    bufferSize = ticksPerBuffer * pd::PdBase::blockSize();

    LOGD("Kortholt constructor: ticksPerBuffer=%d, bufferSize=%d, blockSize=%d",
         ticksPerBuffer, bufferSize, pd::PdBase::blockSize());
    LOGD("Kortholt constructor: CPU cores count=%zu", cpuIds.size());

    pureDataSource = std::make_shared<PureDataSource>(ticksPerBuffer);
    pureDataInputSource = std::make_shared<PureDataInputSource>(ticksPerBuffer);
    errorCallback = std::make_shared<DefaultErrorCallback>(*this);
    outputCallback = std::make_shared<LatencyTuningCallback>();
    inputCallback = std::make_shared<LatencyTuningCallback>();
    outputCallback->setBufferTuneEnabled(false);
    inputCallback->setBufferTuneEnabled(false);
    outputCallback->setCpuIds(cpuIds);
    inputCallback->setCpuIds(std::move(cpuIds));
    outputCallback->setThreadAffinityEnabled(true);
    inputCallback->setThreadAffinityEnabled(true);

    LOGD("Kortholt constructor: Starting initialization");
    start();
}

Kortholt::~Kortholt() {
    // Disable the error callback BEFORE stopping streams. This does two things:
    // 1. Waits for any in-flight onErrorAfterClose() to finish (mutex synchronization)
    // 2. Prevents future callbacks from calling restart() on the dying object
    // Without this, a race exists: Oboe's error callback thread can call restart()
    // → stop() → mutex::lock() after the destructor has already destroyed streamLock.
    errorCallback->disable();
    stop();
    mRetiredStreams.clear();
}

void Kortholt::restart() {
    // Debounce: if both input and output streams disconnect simultaneously
    // (e.g. Bluetooth device removed), both error callbacks fire and call
    // restart(). The atomic flag ensures only the first call proceeds.
    bool expected = false;
    if (!mRestarting.compare_exchange_strong(expected, true)) {
        LOGD("restart: Already restarting, skipping duplicate request");
        return;
    }

    // Clean up streams retired during a previous restart — their error
    // callback threads have long since completed so this is safe.
    mRetiredStreams.clear();

    bool hadInput = mInputEnabled;
    LOGD("restart: hadInput=%s", hadInput ? "true" : "false");

    // Stop existing streams without re-initializing Pure Data.
    // PD state (patch, DSP, receivers) persists — we only need new Oboe streams
    // pointed at the (possibly new) device IDs.
    stop();

    // Recreate the output stream with current device IDs
    {
        std::lock_guard<std::mutex> lock(streamLock);
        auto outputResult = createPlaybackStream();
        if (outputResult != oboe::Result::OK) {
            LOGE("restart: Failed to recreate output stream: %s",
                 oboe::convertToText(outputResult));
            return;
        }

        if (isStream) {
            outputCallback->reset();
            outputCallback->setSource(pureDataSource);
            outputStream->setBufferSizeInFrames(bufferSize * STREAM_BUFFER_MULTIPLIER);
        }
    }

    // Start the output stream
    startStreams();

    // Re-enable mic input if it was active before
    if (hadInput) {
        enableMicInput();
    }

    mRestarting.store(false, std::memory_order_release);
    LOGD("restart: Complete");
}

void Kortholt::setDeviceIds(int32_t inputDeviceId, int32_t outputDeviceId) {
    LOGD("setDeviceIds: inputDeviceId=%d, outputDeviceId=%d (current: input=%d, output=%d)",
         inputDeviceId, outputDeviceId, mInputDeviceId, mOutputDeviceId);

    // Skip restart if device IDs haven't actually changed.
    // This prevents an unnecessary restart on initial preference emission
    // which would race with concurrent libpd message sends.
    if (inputDeviceId == mInputDeviceId && outputDeviceId == mOutputDeviceId) {
        LOGD("setDeviceIds: No change, skipping restart");
        return;
    }

    mInputDeviceId = inputDeviceId;
    mOutputDeviceId = outputDeviceId;
    // Restart streams to apply new device selection
    restart();
}

int32_t Kortholt::saveWaveFile(
        const char *fileName,
        const int32_t duration,
        const char *startBang,
        const char *stopBang
) {
    const int32_t sampleRate = outputStream->getSampleRate();
    const int32_t channelCount = outputStream->getChannelCount();
    const int32_t totalFrames = static_cast<int32_t>(ceil(sampleRate * (duration / 1000.0)));

    // Initialize dr_wav for writing
    // Note: Using 16-bit PCM for compatibility with Android MediaExtractor
    drwav wav;
    drwav_data_format format;
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;  // 16-bit PCM for Android compatibility
    format.channels = channelCount;
    format.sampleRate = sampleRate;
    format.bitsPerSample = 16;  // 16-bit integer PCM

    if (!drwav_init_file_write(&wav, fileName, &format, nullptr)) {
        LOGE("Failed to initialize dr_wav for file: %s", fileName);
        return 0;
    }

    int32_t framesPerBuffer = bufferSize * channelCount;
    auto *floatData = new float[framesPerBuffer];
    auto *int16Data = new int16_t[framesPerBuffer];

    int32_t frameCounter = 0;
    pureDataSource->sendBang(startBang);
    while (frameCounter < totalFrames) {
        int32_t remaining = totalFrames - frameCounter;
        int32_t numFrames = (remaining > bufferSize) ? bufferSize : remaining;
        int32_t samplesToWrite = numFrames * channelCount;

        pureDataSource->renderAudio(floatData, framesPerBuffer);

        // Convert float to int16 for PCM format
        for (int32_t i = 0; i < samplesToWrite; i++) {
            float sample = floatData[i];
            // Clamp to [-1.0, 1.0]
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            int16Data[i] = static_cast<int16_t>(sample * 32767.0f);
        }

        drwav_write_pcm_frames(&wav, numFrames, int16Data);
        frameCounter += numFrames;
    }
    pureDataSource->sendBang(stopBang);

    // Get file size before closing
    drwav_uint64 totalSamples = wav.dataChunkDataSize;

    // Close and finalize WAV file
    drwav_uninit(&wav);

    delete[] floatData;
    delete[] int16Data;

    return static_cast<int32_t>(totalSamples);
}

oboe::Result Kortholt::createPlaybackStream() {
    LOGD("createPlaybackStream: Starting Oboe output stream creation");
    LOGD("createPlaybackStream: bufferSize=%d, ticksPerBuffer=%d, deviceId=%d",
         bufferSize, ticksPerBuffer, mOutputDeviceId);

    oboe::AudioStreamBuilder builder;
    builder.setSharingMode(oboe::SharingMode::Exclusive)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setDirection(oboe::Direction::Output)  // Output for tone generation
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setFormat(oboe::AudioFormat::Float)
            ->setFormatConversionAllowed(true)
            ->setChannelConversionAllowed(true)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setFramesPerDataCallback(bufferSize)
            ->setDataCallback(outputCallback.get())
            ->setErrorCallback(errorCallback.get());

    // Set device ID if specified (not kUnspecified)
    if (mOutputDeviceId != oboe::kUnspecified) {
        builder.setDeviceId(mOutputDeviceId);
        LOGD("createPlaybackStream: Using specific output device: %d", mOutputDeviceId);
    }

    auto result = builder.openStream(outputStream);

    if (result == oboe::Result::OK && outputStream) {
        LOGD("createPlaybackStream: SUCCESS - Output stream opened");
        LOGD("  Sample Rate: %d Hz", outputStream->getSampleRate());
        LOGD("  Channel Count: %d", outputStream->getChannelCount());
        LOGD("  Buffer Size: %d frames", outputStream->getBufferSizeInFrames());
        LOGD("  Format: %s", oboe::convertToText(outputStream->getFormat()));
        LOGD("  Sharing Mode: %s", oboe::convertToText(outputStream->getSharingMode()));
        LOGD("  Performance Mode: %s", oboe::convertToText(outputStream->getPerformanceMode()));
        LOGD("  Direction: %s", oboe::convertToText(outputStream->getDirection()));
    } else {
        LOGE("createPlaybackStream: FAILED - Result: %s", oboe::convertToText(result));
    }

    return result;
}

oboe::Result Kortholt::createRecordingStream() {
    LOGD("createRecordingStream: Starting Oboe input stream creation");
    LOGD("createRecordingStream: bufferSize=%d, ticksPerBuffer=%d, deviceId=%d",
         bufferSize, ticksPerBuffer, mInputDeviceId);

    // Match the input stream's sample rate to the output stream's rate so that
    // Pure Data (which was initialized at the output rate) processes mic data at
    // the correct rate.  If the input hardware doesn't natively support this rate,
    // Oboe's built-in resampler handles conversion transparently while preserving
    // the low-latency MMAP path.
    // See: https://github.com/google/oboe/wiki/FullDuplexStream
    const int32_t targetSampleRate = outputStream ? outputStream->getSampleRate() : 0;

    oboe::AudioStreamBuilder builder;
    builder.setSharingMode(oboe::SharingMode::Exclusive)
            ->setChannelCount(oboe::ChannelCount::Mono)  // Mono input for tuner
            ->setDirection(oboe::Direction::Input)       // Input for microphone
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setFormat(oboe::AudioFormat::Float)
            ->setFormatConversionAllowed(true)
            ->setChannelConversionAllowed(true)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
            ->setFramesPerDataCallback(bufferSize)
            ->setDataCallback(inputCallback.get())
            ->setErrorCallback(errorCallback.get());

    // Match input sample rate to output so PD processes at a consistent rate
    if (targetSampleRate > 0) {
        builder.setSampleRate(targetSampleRate);
        LOGD("createRecordingStream: Setting input sample rate to match output: %d Hz", targetSampleRate);
    }

    // Set device ID if specified (not kUnspecified)
    if (mInputDeviceId != oboe::kUnspecified) {
        builder.setDeviceId(mInputDeviceId);
        LOGD("createRecordingStream: Using specific input device: %d", mInputDeviceId);
    }

    auto result = builder.openStream(inputStream);

    if (result == oboe::Result::OK && inputStream) {
        LOGD("createRecordingStream: SUCCESS - Input stream opened");
        LOGD("  Sample Rate: %d Hz", inputStream->getSampleRate());
        LOGD("  Channel Count: %d", inputStream->getChannelCount());
        LOGD("  Buffer Size: %d frames", inputStream->getBufferSizeInFrames());
        LOGD("  Format: %s", oboe::convertToText(inputStream->getFormat()));
        LOGD("  Sharing Mode: %s", oboe::convertToText(inputStream->getSharingMode()));
        LOGD("  Performance Mode: %s", oboe::convertToText(inputStream->getPerformanceMode()));
        LOGD("  Direction: %s", oboe::convertToText(inputStream->getDirection()));
    } else {
        LOGE("createRecordingStream: FAILED - Result: %s", oboe::convertToText(result));
    }

    return result;
}

void Kortholt::start() {
    std::lock_guard<std::mutex> lock(streamLock);
    LOGD("start: Beginning Kortholt initialization (isStream=%s)", isStream ? "true" : "false");

    // Create output stream for tone generation (no permission required)
    auto outputResult = createPlaybackStream();
    if (outputResult != oboe::Result::OK) {
        LOGE("start: Failed to create output stream: %s", oboe::convertToText(outputResult));
        return;
    }
    LOGD("start: Output stream created successfully");

    // Configure Pure Data with 1 input channel (anticipating future mic input)
    // and set up the input source so PD is ready when mic becomes available.
    // PureDataInputSource provides silence until the input stream is started.
    pureDataSource->setInputChannels(1);
    pureDataSource->setInputSource(pureDataInputSource);

    // Initialize input source using the output stream's sample rate
    // (will be re-used when actual input stream is created)
    if (!pureDataInputSource->init(outputStream->getSampleRate(), 1)) {
        LOGE("start: Failed to initialize PureDataInputSource");
        return;
    }
    LOGD("start: Input source initialized (providing silence until mic enabled)");

    // Initialize Pure Data with output stream settings
    if (!pureDataSource->init(outputStream->getSampleRate(), outputStream->getChannelCount())) {
        LOGE("start: Failed to initialize PureDataSource");
        return;
    }
    LOGD("start: Pure Data source initialized successfully");

    if (isStream) {
        // Configure output callback but do NOT start the stream yet.
        // Streams must be started AFTER the patch is opened to avoid a race
        // between libpd_process_float (audio thread) and libpd_openfile (Java thread).
        outputCallback->reset();
        outputCallback->setSource(pureDataSource);
        outputStream->setBufferSizeInFrames(bufferSize * STREAM_BUFFER_MULTIPLIER);
        LOGD("start: Output stream configured, waiting for startStreams()");
    } else {
        LOGD("start: Configured for file output (not real-time)");
    }
}

void Kortholt::startStreams() {
    std::lock_guard<std::mutex> lock(streamLock);
    LOGD("startStreams: Starting output stream");

    if (!isStream || !outputStream) {
        LOGD("startStreams: No stream to start (isStream=%s, outputStream=%s)",
             isStream ? "true" : "false", outputStream ? "valid" : "null");
        return;
    }

    auto outputStartResult = outputStream->start();
    if (outputStartResult == oboe::Result::OK) {
        LOGD("startStreams: Output stream started successfully");
        LOGD("  Output State: %s", oboe::convertToText(outputStream->getState()));
    } else {
        LOGE("startStreams: Failed to start output stream: %s",
             oboe::convertToText(outputStartResult));
    }
}

void Kortholt::enableMicInput() {
    std::lock_guard<std::mutex> lock(streamLock);
    LOGD("enableMicInput: Creating and starting input stream");

    if (!isStream) {
        LOGD("enableMicInput: Not in stream mode, skipping");
        return;
    }

    // If input stream already exists and is running, skip
    if (inputStream && inputStream->getState() == oboe::StreamState::Started) {
        LOGD("enableMicInput: Input stream already running");
        mInputEnabled = true;
        return;
    }

    // Close existing input stream if any
    stopAndCloseStream(inputStream, "input");
    inputStream.reset();

    // Create the recording stream (requires RECORD_AUDIO permission)
    auto inputResult = createRecordingStream();
    if (inputResult != oboe::Result::OK) {
        LOGE("enableMicInput: Failed to create input stream: %s",
             oboe::convertToText(inputResult));
        return;
    }

    // Configure and start the input stream.
    // This is safe while the output stream is running because
    // PureDataInputSource only writes to a lock-free ring buffer
    // that PureDataSource reads from — no PD state is modified.
    inputCallback->reset();
    inputCallback->setSource(pureDataInputSource);
    inputStream->setBufferSizeInFrames(bufferSize * STREAM_BUFFER_MULTIPLIER);

    auto inputStartResult = inputStream->start();
    if (inputStartResult == oboe::Result::OK) {
        mInputEnabled = true;
        LOGD("enableMicInput: Input stream started successfully");
        LOGD("  Input State: %s", oboe::convertToText(inputStream->getState()));
    } else {
        LOGE("enableMicInput: Failed to start input stream: %s",
             oboe::convertToText(inputStartResult));
    }
}

void Kortholt::stopAndCloseStream(std::shared_ptr<oboe::AudioStream> &stream,
                                   const char *label) {
    if (!stream || stream->getState() == oboe::StreamState::Closed) {
        LOGD("stop: %s stream already closed or null", label);
        return;
    }

    LOGD("stop: %s stream state before stop: %s",
         label, oboe::convertToText(stream->getState()));

    auto stopResult = stream->stop();
    if (stopResult == oboe::Result::OK) {
        LOGD("stop: %s stream stopped successfully", label);
    } else {
        LOGE("stop: Failed to stop %s stream: %s", label, oboe::convertToText(stopResult));
    }

    auto closeResult = stream->close();
    if (closeResult == oboe::Result::OK) {
        LOGD("stop: %s stream closed successfully", label);
    } else {
        LOGE("stop: Failed to close %s stream: %s", label, oboe::convertToText(closeResult));
    }

    // Wait for the stream to fully reach Closed state. On the legacy AudioRecord
    // path (used by some devices like Xiaomi/Poco on Android 13), the system's
    // AudioRecordThread may still be mid-iteration in processAudioBuffer() after
    // close() returns. Destroying the stream before that thread exits causes a
    // SIGSEGV in AudioRecord::isLongTimeZeroData() when it reads the freed buffer.
    auto state = stream->getState();
    if (state != oboe::StreamState::Closed) {
        oboe::StreamState nextState;
        auto waitResult = stream->waitForStateChange(
                state, &nextState, 200 * oboe::kNanosPerMillisecond);
        if (waitResult == oboe::Result::OK) {
            LOGD("stop: %s stream reached state %s after wait",
                 label, oboe::convertToText(nextState));
        } else {
            LOGE("stop: %s stream wait timed out (state=%s), adding safety delay",
                 label, oboe::convertToText(state));
            // Fallback: sleep briefly to let the AudioRecordThread finish its
            // current iteration before we destroy the stream object.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void Kortholt::stop() {
    std::lock_guard<std::mutex> lock(streamLock);
    LOGD("stop: Stopping Kortholt");

    stopAndCloseStream(outputStream, "output");
    stopAndCloseStream(inputStream, "input");

    // Workaround for Oboe bug google/oboe#2325:
    // Don't destroy streams immediately — retire them to keep FilterAudioStream
    // alive while Oboe's error callback thread may still reference it.
    if (outputStream) mRetiredStreams.push_back(std::move(outputStream));
    if (inputStream) mRetiredStreams.push_back(std::move(inputStream));
    LOGD("stop: Kortholt stopped");
}

int32_t Kortholt::calculateTicksPerBuffer() {
    // Calculate buffer size. A multiple of PdBase::blockSize (64) works best.
    auto blockSize = pd::PdBase::blockSize();
    auto framesPerBurst = oboe::DefaultStreamValues::FramesPerBurst;
    float bufferSize = framesPerBurst > blockSize
                       ? framesPerBurst
                       : blockSize * DEFAULT_TICKS_FOR_STREAM;
    int32_t ticksPerBuffer = static_cast<int32_t>(ceil(bufferSize / blockSize)) * 2;
    return ticksPerBuffer;
}

void Kortholt::sendFloat(const char *dest, float value) {
    LOGD("sendFloat: dest=%s, value=%.3f", dest, value);
    if (pureDataSource) {
        pureDataSource->sendFloat(dest, value);
    } else {
        LOGE("sendFloat: pureDataSource is null");
    }
}

void Kortholt::sendBang(const char *dest) {
    LOGD("sendBang: dest=%s", dest);
    if (pureDataSource) {
        pureDataSource->sendBang(dest);
    } else {
        LOGE("sendBang: pureDataSource is null");
    }
}

void Kortholt::sendSymbol(const char *dest, const char *symbol) {
    LOGD("sendSymbol: dest=%s, symbol=%s", dest, symbol);
    if (pureDataSource) {
        pureDataSource->sendSymbol(dest, symbol);
    } else {
        LOGE("sendSymbol: pureDataSource is null");
    }
}

bool Kortholt::openPatch(const char *patch, const char *path) {
    LOGD("openPatch: patch=%s, path=%s", patch, path);
    if (pureDataSource) {
        return pureDataSource->openPatch(patch, path);
    } else {
        LOGE("openPatch: pureDataSource is null");
        return false;
    }
}

void Kortholt::addToSearchPath(const char *path) {
    LOGD("addToSearchPath: path=%s", path);
    if (pureDataSource) {
        pureDataSource->addToSearchPath(path);
    } else {
        LOGE("addToSearchPath: pureDataSource is null");
    }
}

void Kortholt::logPerformanceStatistics() {
    if (pureDataInputSource) {
        auto inputStats = pureDataInputSource->getStatistics();
        LOGD("INPUT PERFORMANCE: %llu frames received, %llu dropped (%.2f%%)",
             static_cast<unsigned long long>(inputStats.totalFramesReceived),
             static_cast<unsigned long long>(inputStats.droppedFrames),
             inputStats.dropoutPercentage);
    }

    if (pureDataSource) {
        auto outputStats = pureDataSource->getStatistics();
        LOGD("OUTPUT PERFORMANCE: %llu callbacks, %llu failed (%.2f%%)",
             static_cast<unsigned long long>(outputStats.totalCallbacks),
             static_cast<unsigned long long>(outputStats.failedCallbacks),
             outputStats.failurePercentage);
    }

    // Oboe stream-level metrics
    std::lock_guard<std::mutex> lock(streamLock);
    if (outputStream) {
        auto latencyResult = outputStream->calculateLatencyMillis();
        auto xRunResult = outputStream->getXRunCount();
        LOGD("OBOE OUTPUT: latency=%.1f ms, xruns=%d, bufferSize=%d frames, state=%s",
             latencyResult ? latencyResult.value() : -1.0,
             xRunResult ? xRunResult.value() : -1,
             outputStream->getBufferSizeInFrames(),
             oboe::convertToText(outputStream->getState()));
    }
    if (inputStream) {
        auto xRunResult = inputStream->getXRunCount();
        LOGD("OBOE INPUT: xruns=%d, bufferSize=%d frames, state=%s",
             xRunResult ? xRunResult.value() : -1,
             inputStream->getBufferSizeInFrames(),
             oboe::convertToText(inputStream->getState()));
    }
}

void Kortholt::setRecorderCallback(AudioRecorderCallback *callback) {
    LOGD("setRecorderCallback: callback=%p", callback);
    if (pureDataInputSource) {
        pureDataInputSource->setRecorderCallback(callback);
        LOGD("setRecorderCallback: Successfully set callback on PureDataInputSource");
    } else {
        LOGE("setRecorderCallback: pureDataInputSource is null");
    }
}

void Kortholt::clearRecorderCallback() {
    LOGD("clearRecorderCallback: Removing recorder callback");
    if (pureDataInputSource) {
        pureDataInputSource->setRecorderCallback(nullptr);
        LOGD("clearRecorderCallback: Successfully cleared callback");
    } else {
        LOGE("clearRecorderCallback: pureDataInputSource is null");
    }
}

int32_t Kortholt::getStreamSampleRate() const {
    std::lock_guard<std::mutex> lock(streamLock);
    if (outputStream) {
        return outputStream->getSampleRate();
    }
    return 0;
}

// JNI bridge functions for Kotlin access
extern "C" {

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeSendBang(JNIEnv *env, jobject instance, jlong kortholtHandle, jstring receiver) {
    if (kortholtHandle == -1) {
        LOGE("nativeSendBang: Invalid kortholt handle");
        return;
    }

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    const char *receiverStr = env->GetStringUTFChars(receiver, nullptr);

    LOGD("nativeSendBang: Calling kortholt->sendBang(%s)", receiverStr);
    kortholt->sendBang(receiverStr);

    env->ReleaseStringUTFChars(receiver, receiverStr);
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeSendFloat(JNIEnv *env, jobject instance, jlong kortholtHandle, jstring receiver, jfloat value) {
    if (kortholtHandle == -1) {
        LOGE("nativeSendFloat: Invalid kortholt handle");
        return;
    }

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    const char *receiverStr = env->GetStringUTFChars(receiver, nullptr);

    LOGD("nativeSendFloat: Calling kortholt->sendFloat(%s, %.3f)", receiverStr, value);
    kortholt->sendFloat(receiverStr, value);

    env->ReleaseStringUTFChars(receiver, receiverStr);
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeSendSymbol(JNIEnv *env, jobject instance, jlong kortholtHandle, jstring receiver, jstring symbol) {
    if (kortholtHandle == -1) {
        LOGE("nativeSendSymbol: Invalid kortholt handle");
        return;
    }

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    const char *receiverStr = env->GetStringUTFChars(receiver, nullptr);
    const char *symbolStr = env->GetStringUTFChars(symbol, nullptr);

    LOGD("nativeSendSymbol: Calling kortholt->sendSymbol(%s, %s)", receiverStr, symbolStr);
    kortholt->sendSymbol(receiverStr, symbolStr);

    env->ReleaseStringUTFChars(receiver, receiverStr);
    env->ReleaseStringUTFChars(symbol, symbolStr);
}

JNIEXPORT jboolean JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeOpenPatch(JNIEnv *env, jobject instance, jlong kortholtHandle, jstring patch, jstring path) {
    if (kortholtHandle == -1) {
        LOGE("nativeOpenPatch: Invalid kortholt handle");
        return false;
    }

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    const char *patchStr = env->GetStringUTFChars(patch, nullptr);
    const char *pathStr = env->GetStringUTFChars(path, nullptr);

    LOGD("nativeOpenPatch: Calling kortholt->openPatch(%s, %s)", patchStr, pathStr);
    bool result = kortholt->openPatch(patchStr, pathStr);

    env->ReleaseStringUTFChars(patch, patchStr);
    env->ReleaseStringUTFChars(path, pathStr);

    return result;
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeAddToSearchPath(JNIEnv *env, jobject instance, jlong kortholtHandle, jstring path) {
    if (kortholtHandle == -1) {
        LOGE("nativeAddToSearchPath: Invalid kortholt handle");
        return;
    }

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    const char *pathStr = env->GetStringUTFChars(path, nullptr);

    LOGD("nativeAddToSearchPath: Calling kortholt->addToSearchPath(%s)", pathStr);
    kortholt->addToSearchPath(pathStr);

    env->ReleaseStringUTFChars(path, pathStr);
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeLogPerformanceStatistics(JNIEnv *env, jobject instance, jlong kortholtHandle) {
    if (kortholtHandle == -1) {
        LOGE("nativeLogPerformanceStatistics: Invalid kortholt handle");
        return;
    }

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    LOGD("nativeLogPerformanceStatistics: Logging performance statistics");
    kortholt->logPerformanceStatistics();
}

}
