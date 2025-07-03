#ifndef PUREDATASOURCE_H
#define PUREDATASOURCE_H

#include <cstdint>
#include <memory>

// Include libpd C headers for global functions  
extern "C" {
    #include "z_libpd.h"
}

// Include Oboe IRenderableAudio interface
#include "./oboe/samples/shared/IRenderableAudio.h"

class PureDataSource : public IRenderableAudio {
private:
    int32_t ticksPerBuffer;

public:
    explicit PureDataSource(int32_t ticksPerBuffer);
    
    void init(int32_t sampleRate, int32_t channelCount);
    
    // IRenderableAudio interface implementation
    void renderAudio(float *audioData, int32_t numFrames) override;
    
    // Message sending functions to Pure Data
    void sendFloat(const char *dest, float value);
    void sendBang(const char *dest);
    void sendSymbol(const char *dest, const char *symbol);
    
    // Patch management functions
    bool openPatch(const char *patch, const char *path);
    void addToSearchPath(const char *path);
};

#endif // PUREDATASOURCE_H
