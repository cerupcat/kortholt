#include "PureDataInputSource.h"
#include <android/log.h>
#include <cstring>
#include <memory>

#define LOG_TAG "PureDataInputSource"
#ifndef LOGD
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

PureDataInputSource::PureDataInputSource(int32_t ticksPerBuffer) :
    ticksPerBuffer(ticksPerBuffer),
    inputBuffer(nullptr),
    inputBufferSize(0),
    inputChannels(0),
    initialized(false) {
    LOGD("PureDataInputSource created with ticksPerBuffer=%d", ticksPerBuffer);
}

PureDataInputSource::~PureDataInputSource() {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (inputBuffer) {
        delete[] inputBuffer;
        inputBuffer = nullptr;
    }
    LOGD("PureDataInputSource destroyed");
}

void PureDataInputSource::init(int32_t sampleRate, int32_t channelCount) {
    std::lock_guard<std::mutex> lock(inputMutex);
    
    LOGD("Initializing PureDataInputSource: sampleRate=%d, channelCount=%d, ticksPerBuffer=%d", 
         sampleRate, channelCount, ticksPerBuffer);
    
    inputChannels = channelCount;
    inputBufferSize = ticksPerBuffer * libpd_blocksize() * channelCount;
    
    // Allocate input buffer
    if (inputBuffer) {
        delete[] inputBuffer;
    }
    inputBuffer = new float[inputBufferSize];
    memset(inputBuffer, 0, inputBufferSize * sizeof(float));
    
    initialized = true;
    LOGD("PureDataInputSource initialization complete - buffer size: %d frames", inputBufferSize);
}

void PureDataInputSource::renderAudio(float *audioData, int32_t numFrames) {
    if (!initialized || !inputBuffer) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(inputMutex);
    
    // Copy the incoming audio data to our input buffer
    int32_t framesToCopy = std::min(numFrames * inputChannels, inputBufferSize);
    memcpy(inputBuffer, audioData, framesToCopy * sizeof(float));
}

void PureDataInputSource::getInputAudio(float *outputBuffer, int32_t numFrames) {
    if (!initialized || !inputBuffer) {
        // No input available, zero out the buffer
        memset(outputBuffer, 0, numFrames * inputChannels * sizeof(float));
        return;
    }
    
    std::lock_guard<std::mutex> lock(inputMutex);
    
    // Copy captured input audio to the output buffer for Pure Data processing
    int32_t framesToCopy = std::min(numFrames * inputChannels, inputBufferSize);
    memcpy(outputBuffer, inputBuffer, framesToCopy * sizeof(float));
} 