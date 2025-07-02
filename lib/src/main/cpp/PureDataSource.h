#ifndef PUREDATASOURCE_H
#define PUREDATASOURCE_H

#include <IRenderableAudio.h>
#include <PdBase.hpp>

class PureDataSource : public IRenderableAudio {
public:
    PureDataSource(int32_t ticksPerBuffer);

    ~PureDataSource() = default;

    void init(int32_t sampleRate, int32_t channelCount);

    void renderAudio(float *audioData, int32_t numFrames) override;

    void sendBang(const char *dest);
    void sendFloat(const char *dest, float value);
    void sendSymbol(const char *dest, const char *symbol);
    
    bool openPatch(const char *patch, const char *path);
    void addToSearchPath(const char *path);

private:
    int32_t ticksPerBuffer;
    std::shared_ptr<pd::PdBase> pdBase;
    std::shared_ptr<pd::PdReceiver> printReceiver;
};

#endif //PUREDATASOURCE_H
