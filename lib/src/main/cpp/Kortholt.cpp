#include <cinttypes>
#include <memory>
#include <cmath>
#include <fstream>
#include <android/log.h>
#include "Kortholt.h"

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

class WaveOutputStream : public WaveFileOutputStream {
public:
    void write(uint8_t b) override {
        mData.push_back(b);
    }

    int32_t length() {
        return (int32_t) mData.size();
    }

    uint8_t *getData() {
        return mData.data();
    }

private:
    std::vector<uint8_t> mData;
};

Kortholt::Kortholt(std::vector<int> cpuIds, bool stream) {
    LOGD("Kortholt constructor: stream=%s", stream ? "true" : "false");
    
    isStream = stream;
    ticksPerBuffer = stream ? calculateTicksPerBuffer() : DEFAULT_TICKS;
    bufferSize = ticksPerBuffer * pd::PdBase::blockSize();
    
    LOGD("Kortholt constructor: ticksPerBuffer=%d, bufferSize=%d, blockSize=%d", 
         ticksPerBuffer, bufferSize, pd::PdBase::blockSize());
    LOGD("Kortholt constructor: CPU cores count=%zu", cpuIds.size());
    
    pureDataSource = std::make_shared<PureDataSource>(ticksPerBuffer);
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

int32_t Kortholt::saveWaveFile(
        const char *fileName,
        const int32_t duration,
        const char *startBang,
        const char *stopBang
) {
    const int32_t sampleRate = outputStream->getSampleRate();
    const int32_t channelCount = outputStream->getChannelCount();
    const int32_t totalFrames = static_cast<int32_t>(ceil(sampleRate * (duration / 1000.0)));

    WaveOutputStream outStream;
    WaveFileWriter writer(&outStream);
    writer.setFrameRate(sampleRate);
    writer.setSamplesPerFrame(channelCount);
    writer.setBitsPerSample(24);

    int32_t framesPerBuffer = bufferSize * channelCount;
    auto *audioData = new float[framesPerBuffer];

    int32_t frameCounter = 0;
    pureDataSource->sendBang(startBang);
    while (frameCounter < totalFrames) {
        int32_t remaining = totalFrames - frameCounter;
        int32_t numFrames = (remaining > bufferSize) ? bufferSize : remaining;
        pureDataSource->renderAudio(audioData, framesPerBuffer);
        writer.write(audioData, 0, numFrames * channelCount);
        frameCounter += numFrames;
    }
    pureDataSource->sendBang(stopBang);
    writer.close();

    if (outStream.length() > 0) {
        auto file = std::ofstream(fileName, std::ios::out | std::ios::binary);
        file.write((char *) outStream.getData(), outStream.length());
        file.close();
    }

    return outStream.length();
}

oboe::Result Kortholt::createPlaybackStream() {
    LOGD("createPlaybackStream: Starting Oboe output stream creation");
    LOGD("createPlaybackStream: bufferSize=%d, ticksPerBuffer=%d", bufferSize, ticksPerBuffer);
    
    oboe::AudioStreamBuilder builder;
    auto result = builder.setSharingMode(oboe::SharingMode::Exclusive)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setDirection(oboe::Direction::Output)  // Output for tone generation
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setFormat(oboe::AudioFormat::Float)
            ->setFormatConversionAllowed(true)
            ->setChannelConversionAllowed(true)
            ->setFramesPerDataCallback(bufferSize)
            ->setDataCallback(outputCallback.get())
            ->setErrorCallback(errorCallback.get())
            ->openStream(outputStream);
    
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
    LOGD("createRecordingStream: bufferSize=%d, ticksPerBuffer=%d", bufferSize, ticksPerBuffer);
    
    oboe::AudioStreamBuilder builder;
    auto result = builder.setSharingMode(oboe::SharingMode::Exclusive)
            ->setChannelCount(oboe::ChannelCount::Mono)  // Mono input for tuner
            ->setDirection(oboe::Direction::Input)       // Input for microphone
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setFormat(oboe::AudioFormat::Float)
            ->setFormatConversionAllowed(true)
            ->setChannelConversionAllowed(true)
            ->setFramesPerDataCallback(bufferSize)
            ->setDataCallback(inputCallback.get())
            ->setErrorCallback(errorCallback.get())
            ->openStream(inputStream);
    
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
            
            // Initialize Pure Data with output stream settings
            pureDataSource->init(outputStream->getSampleRate(), outputStream->getChannelCount());
            
            if (isStream) {
                LOGD("start: Configuring streams for real-time audio");
                
                // Configure output stream
                outputCallback->reset();
                outputCallback->setSource(pureDataSource);
                outputStream->setBufferSizeInFrames(bufferSize);
                
                // Configure input stream - TODO: set source for input processing
                inputCallback->reset();
                // inputCallback->setSource(inputAudioSource); // Future: implement input audio source
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


