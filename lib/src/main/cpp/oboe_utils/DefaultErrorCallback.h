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

#include <mutex>
#include <vector>
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
     */
    void disable() {
        std::lock_guard<std::mutex> lock(mMutex);
        mDisabled = true;
    }

    virtual void onErrorBeforeClose(oboe::AudioStream *oboeStream, oboe::Result error) override {
        LOGE("%s stream error before close: %s",
             oboe::convertToText(oboeStream->getDirection()),
             oboe::convertToText(error));
    }

    virtual void onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) override {
        LOGE("%s stream error after close: %s",
             oboe::convertToText(oboeStream->getDirection()),
             oboe::convertToText(error));

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

};


#endif //SAMPLES_DEFAULT_ERROR_CALLBACK_H
