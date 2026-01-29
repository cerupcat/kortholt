#include <jni.h>
#include <string>
#include <memory>
#include <android/log.h>
#include "OboeAudioRecorderNative.h"
#include "PureDataInputSource.h"

#define LOG_TAG "OboeAudioRecorderJNI"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Global recorder instance
// Note: In production, you might want to use a handle system if multiple recorders are needed
static std::unique_ptr<OboeAudioRecorderNative> gRecorder;

extern "C" {

/**
 * Initialize the native audio recorder.
 * Should be called once when the recorder module is loaded.
 */
JNIEXPORT jlong JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativeCreate(
        JNIEnv* env,
        jobject /* this */) {
    LOGD("nativeCreate called");

    try {
        auto* recorder = new OboeAudioRecorderNative();
        return reinterpret_cast<jlong>(recorder);
    } catch (const std::exception& e) {
        LOGE("Failed to create recorder: %s", e.what());
        return 0;
    }
}

/**
 * Start recording to a WAV file.
 *
 * @param recorderHandle Native recorder handle from nativeCreate
 * @param filePath Output file path
 * @param sampleRate Sample rate in Hz
 * @param channelCount Number of channels (1 or 2)
 * @param bitsPerSample Bits per sample (16 or 24)
 * @return true if recording started successfully
 */
JNIEXPORT jboolean JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativeStartRecording(
        JNIEnv* env,
        jobject /* this */,
        jlong recorderHandle,
        jstring filePath,
        jint sampleRate,
        jint channelCount,
        jint bitsPerSample) {

    if (recorderHandle == 0) {
        LOGE("Invalid recorder handle");
        return JNI_FALSE;
    }

    auto* recorder = reinterpret_cast<OboeAudioRecorderNative*>(recorderHandle);

    // Convert Java string to C++ string
    const char* filePathStr = env->GetStringUTFChars(filePath, nullptr);
    if (!filePathStr) {
        LOGE("Failed to get file path string");
        return JNI_FALSE;
    }

    std::string filePathCpp(filePathStr);
    env->ReleaseStringUTFChars(filePath, filePathStr);

    LOGD("nativeStartRecording: path=%s, sr=%d, ch=%d, bits=%d",
         filePathCpp.c_str(), sampleRate, channelCount, bitsPerSample);

    // Start recording
    bool success = recorder->startRecording(
        filePathCpp,
        static_cast<int32_t>(sampleRate),
        static_cast<int32_t>(channelCount),
        static_cast<int32_t>(bitsPerSample)
    );

    return success ? JNI_TRUE : JNI_FALSE;
}

/**
 * Stop recording and finalize the WAV file.
 *
 * @param recorderHandle Native recorder handle
 * @return true if stopped successfully
 */
JNIEXPORT jboolean JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativeStopRecording(
        JNIEnv* env,
        jobject /* this */,
        jlong recorderHandle) {

    if (recorderHandle == 0) {
        LOGE("Invalid recorder handle");
        return JNI_FALSE;
    }

    auto* recorder = reinterpret_cast<OboeAudioRecorderNative*>(recorderHandle);

    LOGD("nativeStopRecording called");

    bool success = recorder->stopRecording();
    return success ? JNI_TRUE : JNI_FALSE;
}

/**
 * Pause recording.
 */
JNIEXPORT jboolean JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativePauseRecording(
        JNIEnv* env,
        jobject /* this */,
        jlong recorderHandle) {

    if (recorderHandle == 0) {
        LOGE("Invalid recorder handle");
        return JNI_FALSE;
    }

    auto* recorder = reinterpret_cast<OboeAudioRecorderNative*>(recorderHandle);

    LOGD("nativePauseRecording called");

    bool success = recorder->pauseRecording();
    return success ? JNI_TRUE : JNI_FALSE;
}

/**
 * Resume recording after pause.
 */
JNIEXPORT jboolean JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativeResumeRecording(
        JNIEnv* env,
        jobject /* this */,
        jlong recorderHandle) {

    if (recorderHandle == 0) {
        LOGE("Invalid recorder handle");
        return JNI_FALSE;
    }

    auto* recorder = reinterpret_cast<OboeAudioRecorderNative*>(recorderHandle);

    LOGD("nativeResumeRecording called");

    bool success = recorder->resumeRecording();
    return success ? JNI_TRUE : JNI_FALSE;
}

/**
 * Check if currently recording.
 */
JNIEXPORT jboolean JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativeIsRecording(
        JNIEnv* env,
        jobject /* this */,
        jlong recorderHandle) {

    if (recorderHandle == 0) {
        return JNI_FALSE;
    }

    auto* recorder = reinterpret_cast<OboeAudioRecorderNative*>(recorderHandle);
    return recorder->isRecording() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Check if paused.
 */
JNIEXPORT jboolean JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativeIsPaused(
        JNIEnv* env,
        jobject /* this */,
        jlong recorderHandle) {

    if (recorderHandle == 0) {
        return JNI_FALSE;
    }

    auto* recorder = reinterpret_cast<OboeAudioRecorderNative*>(recorderHandle);
    return recorder->isPaused() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Destroy the native recorder and free resources.
 */
JNIEXPORT void JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativeDestroy(
        JNIEnv* env,
        jobject /* this */,
        jlong recorderHandle) {

    if (recorderHandle == 0) {
        return;
    }

    LOGD("nativeDestroy called");

    auto* recorder = reinterpret_cast<OboeAudioRecorderNative*>(recorderHandle);
    delete recorder;
}

/**
 * Register the recorder as a callback with PureDataInputSource.
 * This should be called from Kortholt initialization code.
 */
JNIEXPORT void JNICALL
Java_com_affinityblue_tunable_recorder_OboeAudioRecorder_nativeRegisterWithInputSource(
        JNIEnv* env,
        jobject /* this */,
        jlong recorderHandle,
        jlong inputSourceHandle) {

    if (recorderHandle == 0 || inputSourceHandle == 0) {
        LOGE("Invalid handles for registration");
        return;
    }

    auto* recorder = reinterpret_cast<OboeAudioRecorderNative*>(recorderHandle);
    auto* inputSource = reinterpret_cast<PureDataInputSource*>(inputSourceHandle);

    LOGD("Registering recorder with input source");

    // TODO: Add setRecorderCallback method to PureDataInputSource
    // inputSource->setRecorderCallback(recorder);
}

} // extern "C"
