#include <cinttypes>
#include <memory>
#include <cmath>
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
    outputCallback->setCpuIds(cpuIds);
    inputCallback->setCpuIds(std::move(cpuIds));
    outputCallback->setThreadAffinityEnabled(true);
    inputCallback->setThreadAffinityEnabled(true);

    LOGD("Kortholt constructor: Starting initialization");
    start();
}

Kortholt::~Kortholt() {
    stop();
}

void Kortholt::restart() {
    stop();
    start();
}

void Kortholt::setDeviceIds(int32_t inputDeviceId, int32_t outputDeviceId) {
    LOGD("setDeviceIds: inputDeviceId=%d, outputDeviceId=%d", inputDeviceId, outputDeviceId);
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

    oboe::AudioStreamBuilder builder;
    builder.setSharingMode(oboe::SharingMode::Exclusive)
            ->setChannelCount(oboe::ChannelCount::Mono)  // Mono input for tuner
            ->setDirection(oboe::Direction::Input)       // Input for microphone
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setFormat(oboe::AudioFormat::Float)
            ->setFormatConversionAllowed(true)
            ->setChannelConversionAllowed(true)
            ->setFramesPerDataCallback(bufferSize)
            ->setDataCallback(inputCallback.get())
            ->setErrorCallback(errorCallback.get());

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

    // Create output stream for tone generation
    auto outputResult = createPlaybackStream();
    if (outputResult == oboe::Result::OK) {
        LOGD("start: Output stream created successfully");

        // Create input stream for tuner
        auto inputResult = createRecordingStream();
        if (inputResult == oboe::Result::OK) {
            LOGD("start: Both streams created successfully, initializing Pure Data");

            // Configure Pure Data with input channel count and set input source
            pureDataSource->setInputChannels(inputStream->getChannelCount());
            pureDataSource->setInputSource(pureDataInputSource);

            // Initialize input source with input stream settings (must be done first)
            if (!pureDataInputSource->init(inputStream->getSampleRate(), inputStream->getChannelCount())) {
                LOGE("start: Failed to initialize PureDataInputSource");
                return;
            }
            LOGD("start: Input source initialized successfully");

            // Initialize Pure Data with output stream settings
            if (!pureDataSource->init(outputStream->getSampleRate(), outputStream->getChannelCount())) {
                LOGE("start: Failed to initialize PureDataSource");
                return;
            }
            LOGD("start: Pure Data source initialized successfully");

            if (isStream) {
                LOGD("start: Configuring streams for real-time audio");

                // Configure output stream
                outputCallback->reset();
                outputCallback->setSource(pureDataSource);
                outputStream->setBufferSizeInFrames(bufferSize);

                // Configure input stream with input audio source
                inputCallback->reset();
                inputCallback->setSource(pureDataInputSource);
                inputStream->setBufferSizeInFrames(bufferSize);

                // Start both streams
                auto outputStartResult = outputStream->start();
                auto inputStartResult = inputStream->start();

                if (outputStartResult == oboe::Result::OK && inputStartResult == oboe::Result::OK) {
                    LOGD("start: Both streams started successfully");
                    LOGD("  Output State: %s", oboe::convertToText(outputStream->getState()));
                    LOGD("  Input State: %s", oboe::convertToText(inputStream->getState()));
                } else {
                    LOGE("start: Failed to start streams - Output: %s, Input: %s",
                         oboe::convertToText(outputStartResult), oboe::convertToText(inputStartResult));
                }
            } else {
                LOGD("start: Streams configured for file output (not real-time)");
            }
        } else {
            LOGE("start: Failed to create input stream: %s", oboe::convertToText(inputResult));
        }
    } else {
        LOGE("start: Failed to create output stream: %s", oboe::convertToText(outputResult));
    }
}

void Kortholt::stop() {
    std::lock_guard<std::mutex> lock(streamLock);
    LOGD("stop: Stopping Kortholt");

    // Stop output stream
    if (outputStream && outputStream->getState() != oboe::StreamState::Closed) {
        LOGD("stop: Output stream state before stop: %s", oboe::convertToText(outputStream->getState()));
        auto stopResult = outputStream->stop();
        if (stopResult == oboe::Result::OK) {
            LOGD("stop: Output stream stopped successfully");
        } else {
            LOGE("stop: Failed to stop output stream: %s", oboe::convertToText(stopResult));
        }

        auto closeResult = outputStream->close();
        if (closeResult == oboe::Result::OK) {
            LOGD("stop: Output stream closed successfully");
        } else {
            LOGE("stop: Failed to close output stream: %s", oboe::convertToText(closeResult));
        }
    } else {
        LOGD("stop: Output stream already closed or null");
    }

    // Stop input stream
    if (inputStream && inputStream->getState() != oboe::StreamState::Closed) {
        LOGD("stop: Input stream state before stop: %s", oboe::convertToText(inputStream->getState()));
        auto stopResult = inputStream->stop();
        if (stopResult == oboe::Result::OK) {
            LOGD("stop: Input stream stopped successfully");
        } else {
            LOGE("stop: Failed to stop input stream: %s", oboe::convertToText(stopResult));
        }

        auto closeResult = inputStream->close();
        if (closeResult == oboe::Result::OK) {
            LOGD("stop: Input stream closed successfully");
        } else {
            LOGE("stop: Failed to close input stream: %s", oboe::convertToText(closeResult));
        }
    } else {
        LOGD("stop: Input stream already closed or null");
    }

    outputStream.reset();
    inputStream.reset();
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


