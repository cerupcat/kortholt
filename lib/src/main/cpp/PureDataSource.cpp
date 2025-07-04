#include "PureDataSource.h"
#include "PureDataInputSource.h"
#include <android/log.h>
#include <cstring>
#include <memory>

#define LOG_TAG "PureDataSource"
#ifndef LOGD
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

extern "C" void externals_setup(void);

PureDataSource::PureDataSource(int32_t ticksPerBuffer) :
    ticksPerBuffer(ticksPerBuffer),
    inputChannels(0),
    outputChannels(2),
    inputSource(nullptr) {
    // No longer create separate pd::PdBase instance - use global libpd
    LOGD("PureDataSource created to use global libpd instance");
}

void PureDataSource::init(int32_t sampleRate, int32_t channelCount) {
    std::lock_guard<std::mutex> lock(processingMutex);
    
    LOGD("Initializing Pure Data: sampleRate=%d, channelCount=%d (output), inputChannels=%d, ticksPerBuffer=%d", 
         sampleRate, channelCount, inputChannels, ticksPerBuffer);
    
    outputChannels = channelCount;
    
    // Initialize libpd's audio processing system with input and output channels
    // This is required for libpd_process_float to work properly
    LOGD("Initializing libpd audio processing system...");
    int initResult = libpd_init_audio(inputChannels, outputChannels, sampleRate);
    if (initResult != 0) {
        LOGE("Failed to initialize libpd audio system: %d", initResult);
        return;
    }
    LOGD("libpd audio system initialized successfully with %d input and %d output channels", 
         inputChannels, outputChannels);
    
    // Enable DSP computation using the correct libpd message system
    // This is equivalent to [; pd dsp 1(
    LOGD("Enabling DSP computation...");
    libpd_start_message(1); // one entry in list
    libpd_add_float(1.0f);  // dsp on
    libpd_finish_message("pd", "dsp");
    LOGD("DSP computation enabled");

    // Setup externals
    LOGD("Setting up externals...");
    externals_setup();
    LOGD("Externals setup complete");
    
    LOGD("PureDataSource initialization complete");
}

void PureDataSource::setInputChannels(int32_t channels) {
    std::lock_guard<std::mutex> lock(processingMutex);
    inputChannels = channels;
    LOGD("Input channels set to %d", inputChannels);
}

void PureDataSource::setInputSource(std::shared_ptr<PureDataInputSource> source) {
    std::lock_guard<std::mutex> lock(processingMutex);
    inputSource = source;
    LOGD("Input source set");
}

void PureDataSource::renderAudio(float *audioData, int32_t numFrames) {
    // Process audio using global libpd - output only (for tone generation)
    int ticks = numFrames / libpd_blocksize();
    
    // Prepare input buffer
    float* inputBuffer = nullptr;
    if (inputChannels > 0 && inputSource) {
        inputBuffer = new float[numFrames * inputChannels];
        inputSource->getInputAudio(inputBuffer, numFrames);
    }
    
    // Process with Pure Data
    libpd_process_float(ticks, inputBuffer, audioData);
    
    // Clean up
    if (inputBuffer) {
        delete[] inputBuffer;
    }
}

void PureDataSource::processAudio(float *inputData, float *outputData, int32_t numFrames) {
    std::lock_guard<std::mutex> lock(processingMutex);
    
    // Process audio using global libpd with both input and output
    int ticks = numFrames / libpd_blocksize();
    libpd_process_float(ticks, inputData, outputData);
}

void PureDataSource::sendFloat(const char *dest, float value) {
    LOGD("Sending float to %s: %f", dest, value);
    libpd_float(dest, value);
}

void PureDataSource::sendBang(const char *dest) {
    LOGD("Sending bang to %s", dest);
    libpd_bang(dest);
}

void PureDataSource::sendSymbol(const char *dest, const char *symbol) {
    LOGD("Sending symbol to %s: %s", dest, symbol);
    libpd_symbol(dest, symbol);
}

bool PureDataSource::openPatch(const char *patch, const char *path) {
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
    LOGD("Adding to search path: %s", path);
    libpd_add_to_search_path(path);
}


