/*
 * Copyright 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SAMPLES_DEFAULT_ERROR_CALLBACK_H
#define SAMPLES_DEFAULT_ERROR_CALLBACK_H

#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <thread>
#include <oboe/AudioStreamCallback.h>
#include <logging_macros.h>

#include "IRestartable.h"

/**
 * This is a callback object which will be called when a stream error occurs.
 *
 * It is constructed using an `IRestartable` which allows it to automatically restart the parent
 * object if the stream is disconnected (for example, when headphones are attached).
 *
 * @param IRestartable - the object which should be restarted when the stream is disconnected
 */
class DefaultErrorCallback : public oboe::AudioStreamErrorCallback {
public:

    DefaultErrorCallback(IRestartable &parent): mParent(parent) {}
    virtual ~DefaultErrorCallback() = default;

    /**
     * Disable the callback and wait for any in-flight invocation to complete.
     * After this returns, onErrorAfterClose will never call mParent.restart().
     *
     * Must be called before the parent (IRestartable) is destroyed to prevent
     * a use-after-free race between the Oboe error callback thread and the
     * destructor thread.
     *
     * Sets an atomic flag FIRST (lock-free barrier for late-arriving threads)
     * and then acquires the mutex to synchronize with any in-flight callback.
     */
    void disable() {
        // Set atomic flag first — late-arriving Oboe threads will see this
        // even if they arrive after ~Kortholt() has destroyed mMutex.
        mAtomicDisabled.store(true, std::memory_order_release);

        std::lock_guard<std::mutex> lock(mMutex);
        mDisabled = true;
    }

    virtual void onErrorBeforeClose(oboe::AudioStream *oboeStream, oboe::Result error) override {
        LOGE("%s stream error before close: %s",
             oboe::convertToText(oboeStream->getDirection()),
             oboe::convertToText(error));

        // Workaround for AudioRecord::isLongTimeZeroData SIGSEGV (Android 13+):
        // Oboe calls stop() then close() during error handling. close() unmaps the
        // shared audio buffer, but AudioRecordThread may still be inside
        // processAudioBuffer() -> isLongTimeZeroData(). This delay (between Oboe's
        // internal stop and the close that follows this callback) gives the thread
        // time to exit. Same rationale as Kortholt::stopAndCloseStream.
        if (oboeStream->getDirection() == oboe::Direction::Input) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            LOGI("Input stream pre-close delay complete (isLongTimeZeroData workaround)");
        }
    }

    virtual void onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) override {
        LOGE("%s stream error after close: %s",
             oboe::convertToText(oboeStream->getDirection()),
             oboe::convertToText(error));

        // Lock-free early exit: check the atomic flag BEFORE acquiring mMutex.
        // This prevents a crash when an Oboe error callback thread arrives after
        // ~Kortholt() has destroyed mMutex during member destruction.
        // The atomic flag is set in disable() before stop() begins, so any
        // callback thread spawned during stream teardown will see it.
        if (mAtomicDisabled.load(std::memory_order_acquire)) {
            LOGI("Skipping callback for %s (disabled, lock-free check)",
                 oboe::convertToText(oboeStream->getDirection()));
            return;
        }

        if (error == oboe::Result::ErrorDisconnected) {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mDisabled) {
                LOGI("Skipping restart after %s disconnect (callback disabled)",
                     oboe::convertToText(oboeStream->getDirection()));
                return;
            }
            LOGI("Restarting AudioStream after %s disconnect",
                 oboe::convertToText(oboeStream->getDirection()));
            mParent.restart();
        }
    }

private:
    IRestartable &mParent;
    std::mutex mMutex;
    bool mDisabled = false;
    std::atomic<bool> mAtomicDisabled{false};

};


#endif //SAMPLES_DEFAULT_ERROR_CALLBACK_H
