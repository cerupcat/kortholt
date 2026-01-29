#ifndef AUDIORECORDERCALLBACK_H
#define AUDIORECORDERCALLBACK_H

#include <cstdint>

/**
 * Callback interface for receiving audio data from PureDataInputSource.
 *
 * This allows the audio recorder to tap into the existing microphone input stream
 * that's already being used for Pure Data pitch detection.
 *
 * IMPORTANT: Implementations MUST be real-time safe:
 * - No memory allocation
 * - No locks (use lock-free data structures)
 * - No system calls or I/O
 * - Keep processing time minimal (<1ms)
 */
class AudioRecorderCallback {
public:
    virtual ~AudioRecorderCallback() = default;

    /**
     * Called from the real-time audio input thread with raw PCM audio data.
     *
     * @param audioData Interleaved float PCM samples in range [-1.0, 1.0]
     * @param numFrames Number of frames (samples per channel)
     * @param channelCount Number of audio channels
     *
     * NOTE: This runs on the audio thread - keep it FAST and lock-free!
     */
    virtual void onAudioData(const float* audioData, int32_t numFrames, int32_t channelCount) = 0;
};

#endif // AUDIORECORDERCALLBACK_H
