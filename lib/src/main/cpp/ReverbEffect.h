#ifndef REVERBEFFECT_H
#define REVERBEFFECT_H

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <android/log.h>
#include "PlateReverb.hpp"

#define REVERB_LOG_TAG "ReverbEffect"
#define REVERB_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, REVERB_LOG_TAG, __VA_ARGS__)

/**
 * Real-time safe reverb effect wrapper around dsp-lib PlateReverb.
 *
 * Thread safety:
 * - setEnabled() / setLevel() can be called from any thread (atomic)
 * - process() runs on the real-time audio thread (lock-free)
 * - setSampleRate() must be called before processing, NOT on audio thread
 */
class ReverbEffect {
public:
    ReverbEffect() {
        // Set sensible defaults for the plate reverb
        reverb_.setSize(0.5f);       // Medium room
        reverb_.setPredelay(0.01f);  // 10ms predelay
        reverb_.setLowpass(8000.0f); // Gentle high-frequency rolloff
        // Other params set by setLevel()
        setLevel(0.3f);  // Match default preference
    }

    /**
     * Initialize with sample rate. Must be called before process().
     * NOT real-time safe (allocates internally).
     */
    void setSampleRate(int32_t sampleRate) {
        REVERB_LOGD("setSampleRate: %d", sampleRate);
        reverb_.setSampleRate(static_cast<float>(sampleRate));
    }

    /**
     * Enable or disable reverb processing.
     * Safe to call from any thread.
     */
    void setEnabled(bool enabled) {
        REVERB_LOGD("setEnabled: %s", enabled ? "true" : "false");
        enabled_.store(enabled, std::memory_order_release);
    }

    /**
     * Set reverb level (0.0 to 1.0).
     * Maps single value to multiple PlateReverb parameters.
     * Safe to call from any thread.
     */
    void setLevel(float level) {
        level = std::clamp(level, 0.0f, 1.0f);
        REVERB_LOGD("setLevel: %.2f", level);
        level_.store(level, std::memory_order_release);

        // Map level to PlateReverb parameters:
        // Mix: 0.0 -> 0.0 (dry), 1.0 -> 0.8 (mostly wet, never fully wet)
        float mix = level * 0.8f;
        reverb_.setMix(mix);

        // Decay: 0.0 -> 0.3 (short), 1.0 -> 0.85 (long tail)
        float decay = 0.3f + (level * 0.55f);
        reverb_.setDecay(decay);

        // Damping: 0.0 -> 3000 Hz (dark), 1.0 -> 10000 Hz (bright)
        float damping = 3000.0f + (level * 7000.0f);
        reverb_.setDamping(damping);
    }

    /**
     * Process audio buffer in-place. Mono input.
     * REAL-TIME SAFE: No allocations, no locks, no I/O.
     *
     * @param audioData Interleaved float PCM samples (modified in-place)
     * @param numFrames Number of frames
     * @param channels Number of channels (expected 1 for mono)
     */
    void process(float* audioData, int32_t numFrames, int32_t channels) {
        if (!enabled_.load(std::memory_order_acquire)) {
            return;  // Pass through unchanged
        }

        // PlateReverb processes sample-by-sample with stereo I/O.
        // For mono: feed same sample to both L/R, take left output.
        if (channels == 1) {
            for (int32_t i = 0; i < numFrames; ++i) {
                float leftOut, rightOut;
                reverb_.process(audioData[i], audioData[i], &leftOut, &rightOut);
                audioData[i] = leftOut;
            }
        } else {
            // Stereo: process interleaved pairs
            for (int32_t i = 0; i < numFrames * channels; i += channels) {
                float leftOut, rightOut;
                reverb_.process(audioData[i], audioData[i + 1], &leftOut, &rightOut);
                audioData[i] = leftOut;
                audioData[i + 1] = rightOut;
            }
        }
    }

    bool isEnabled() const {
        return enabled_.load(std::memory_order_acquire);
    }

    float getLevel() const {
        return level_.load(std::memory_order_acquire);
    }

private:
    PlateReverb<float, int> reverb_;
    std::atomic<bool> enabled_{false};
    std::atomic<float> level_{0.3f};
};

#endif // REVERBEFFECT_H
