#ifndef PUREDATASOURCE_H
#define PUREDATASOURCE_H

#include <cstdint>
#include <memory>
#include <mutex>

// Include libpd C headers for global functions  
extern "C" {
    #include "z_libpd.h"
}

// Include Oboe IRenderableAudio interface
#include "./oboe/samples/shared/IRenderableAudio.h"

// Forward declaration
class PureDataInputSource;

class PureDataSource : public IRenderableAudio {
private:
    int32_t ticksPerBuffer;
    int32_t inputChannels;
    int32_t outputChannels;
    std::shared_ptr<PureDataInputSource> inputSource;
    std::mutex processingMutex;

public:
    explicit PureDataSource(int32_t ticksPerBuffer);
    
    void init(int32_t sampleRate, int32_t channelCount);
    void setInputChannels(int32_t inputChannels);
    void setInputSource(std::shared_ptr<PureDataInputSource> inputSource);
    
    // IRenderableAudio interface implementation (for output)
    void renderAudio(float *audioData, int32_t numFrames) override;
    
    // Process audio with both input and output
    void processAudio(float *inputData, float *outputData, int32_t numFrames);
    
    // Message sending functions to Pure Data
    void sendFloat(const char *dest, float value);
    void sendBang(const char *dest);
    void sendSymbol(const char *dest, const char *symbol);
    
    // Patch management functions
    bool openPatch(const char *patch, const char *path);
    void addToSearchPath(const char *path);
};

#endif // PUREDATASOURCE_H
