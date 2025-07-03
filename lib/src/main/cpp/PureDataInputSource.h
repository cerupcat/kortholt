#ifndef PUREDATAINPUTSOURCE_H
#define PUREDATAINPUTSOURCE_H

#include <cstdint>
#include <memory>
#include <mutex>

// Include libpd C headers for global functions  
extern "C" {
    #include "z_libpd.h"
}

// Include Oboe IRenderableAudio interface
#include "./oboe/samples/shared/IRenderableAudio.h"

class PureDataInputSource : public IRenderableAudio {
private:
    int32_t ticksPerBuffer;
    std::mutex inputMutex;
    float* inputBuffer;
    int32_t inputBufferSize;
    int32_t inputChannels;
    bool initialized;

public:
    explicit PureDataInputSource(int32_t ticksPerBuffer);
    virtual ~PureDataInputSource();
    
    void init(int32_t sampleRate, int32_t channelCount);
    
    // IRenderableAudio interface implementation - receives input audio data
    void renderAudio(float *audioData, int32_t numFrames) override;
    
    // Get the captured input data for Pure Data processing
    void getInputAudio(float *outputBuffer, int32_t numFrames);
    
    // Check if input is available
    bool hasInputData() const { return initialized && inputBuffer != nullptr; }
};

#endif // PUREDATAINPUTSOURCE_H 