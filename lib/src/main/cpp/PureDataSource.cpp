#include "PureDataSource.h"
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <string>

#define LOG_TAG "PureDataSource"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Custom print receiver to capture Pure Data's output and error messages
class PdPrintReceiver : public pd::PdReceiver {
public:
    void print(const std::string& message) {
        LOGD("PD Print: %s", message.c_str());
    }
};

PureDataSource::PureDataSource(int32_t ticksPerBuffer) {
    this->ticksPerBuffer = ticksPerBuffer;
    pdBase = std::make_shared<pd::PdBase>();
    
    // Set up print receiver to capture PD messages
    printReceiver = std::make_shared<PdPrintReceiver>();
    pdBase->setReceiver(printReceiver.get());
    LOGD("PureDataSource created with print receiver");
}

void PureDataSource::init(int32_t sampleRate, int32_t channelCount) {
    LOGD("Initializing Pure Data: sampleRate=%d, channelCount=%d, ticksPerBuffer=%d", 
         sampleRate, channelCount, ticksPerBuffer);
    
    LOGD("About to call pdBase->init()...");
    bool initResult = pdBase->init(0, channelCount, sampleRate, false);
    LOGD("pdBase->init() returned: %s", initResult ? "true" : "false");
    
    if (initResult) {
        LOGD("Calling pdBase->computeAudio(true)...");
        pdBase->computeAudio(true);
        LOGD("Pure Data initialized successfully");
        LOGD("Pure Data initialization complete - print receiver active");
        
        // Test the print receiver by sending a message to Pure Data
        LOGD("Testing print receiver...");
        pdBase->sendSymbol("pd", "version");
    } else {
        LOGE("Failed to initialize Pure Data");
    }
}

void PureDataSource::renderAudio(float *audioData, int32_t numFrames) {
    // Create proper input buffer for Pure Data - zero-filled for tone generation
    static float* inputBuffer = nullptr;
    static int32_t lastBufferSize = 0;
    static int debugCounter = 0;
    
    int32_t bufferSize = ticksPerBuffer * 64; // PD uses 64-sample blocks
    
    // Allocate input buffer if needed
    if (inputBuffer == nullptr || bufferSize != lastBufferSize) {
        if (inputBuffer != nullptr) {
            delete[] inputBuffer;
        }
        inputBuffer = new float[bufferSize]();  // Zero-initialized
        lastBufferSize = bufferSize;
        LOGD("Allocated input buffer: size=%d", bufferSize);
    }
    
    // Clear output buffer first
    memset(audioData, 0, numFrames * 2 * sizeof(float)); // stereo
    
    // Debug Pure Data state
    if (debugCounter % 240 == 0) { // Every ~5 seconds
        LOGD("PD Debug - ticksPerBuffer: %d, bufferSize: %d, numFrames: %d", 
             ticksPerBuffer, bufferSize, numFrames);
        LOGD("PD Debug - pdBase initialized: %s", pdBase ? "YES" : "NO");
    }
    
    // Process audio through Pure Data
    bool result = pdBase->processFloat(ticksPerBuffer, inputBuffer, audioData);
    
    // Debug: Check if we're getting any output
    if (debugCounter++ % 480 == 0) { // Log every ~10 seconds at 48kHz
        float maxSample = 0.0f;
        for (int i = 0; i < numFrames * 2; i++) { // stereo
            if (abs(audioData[i]) > maxSample) {
                maxSample = abs(audioData[i]);
            }
        }
        LOGD("processFloat result: %s, frames: %d, max sample: %.6f", 
             result ? "SUCCESS" : "FAILED", numFrames, maxSample);
             
        // Sample a few output values for debugging
        LOGD("Sample values: [0]=%.6f, [1]=%.6f, [2]=%.6f, [3]=%.6f", 
             audioData[0], audioData[1], audioData[2], audioData[3]);
    }
}

void PureDataSource::sendBang(const char *dest) {
    pdBase->sendBang(dest);
}

void PureDataSource::sendFloat(const char *dest, float value) {
    pdBase->sendFloat(dest, value);
}

void PureDataSource::sendSymbol(const char *dest, const char *symbol) {
    pdBase->sendSymbol(dest, symbol);
}


