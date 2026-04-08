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
        // Oboe's error flow calls onErrorBeforeClose → close(). close() internally
        // calls stop() + release() which unmaps the shared audio buffer
        // (mCblkMemory, mBufferMemory). If AudioRecordThread is still inside
        // processAudioBuffer() → isLongTimeZeroData() at that moment, it reads
        // the unmapped address → SIGSEGV.
        //
        // The critical difference from Kortholt::stopAndCloseStream (which works
        // correctly) is that HERE stop() has NOT been called yet — Oboe calls
        // stop() inside close(), AFTER this callback returns. Without an explicit
        // requestStop() the AudioRecordThread is still actively running during
        // the sleep, and the sleep achieves nothing.
        //
        // Fix: call requestStop() to signal AudioRecordThread to exit its loop,
        // THEN sleep to let it finish its current processAudioBuffer() iteration,
        // THEN return so Oboe's close() can safely unmap the buffer.
        //
        // IMPORTANT: We use 200ms here (vs 50ms in Kortholt::stopAndCloseStream)
        // because requestStop() is non-blocking and may NOT interrupt the
        // AudioRecordThread's blocking obtainBuffer() call. In the normal
        // teardown path, the blocking stop() calls AudioRecord::stop() which
        // calls mProxy->interrupt() to wake the thread immediately. Here,
        // requestStop() only sets mActive=false — the thread won't check that
        // flag until obtainBuffer() times out (up to ~200ms on the legacy
        // AudioRecord path). We can't use the blocking stop() here because it
        // could deadlock with Oboe's error handling thread.
        if (oboeStream->getDirection() == oboe::Direction::Input) {
            oboeStream->requestStop();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
