#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include "Kortholt.h"
#include "OboeAudioRecorderNative.h"

#define LOG_TAG "JNI_Bridge"
// Use Oboe's existing logging macros to avoid redefinition
#ifndef LOGD
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif

std::vector<int> convertJavaArrayToVector(
        JNIEnv *env,
        jintArray intArray
) {
    std::vector<int> v;
    jsize length = env->GetArrayLength(intArray);
    if (length > 0) {
        jint *elements = env->GetIntArrayElements(intArray, nullptr);
        v.insert(v.end(), &elements[0], &elements[length]);
        env->ReleaseIntArrayElements(intArray, elements, 0);
    }
    return v;
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeCreateKortholt(
        JNIEnv *env,
        jobject /*unused*/,
        jintArray jCpuIds,
        jboolean stream,
        jint inputDeviceId,
        jint outputDeviceId
) {
    std::vector<int> cpuIds = convertJavaArrayToVector(env, jCpuIds);
    LOGD("nativeCreateKortholt: stream=%s, cpuIds.size=%zu, inputDeviceId=%d, outputDeviceId=%d",
         stream ? "true" : "false", cpuIds.size(), inputDeviceId, outputDeviceId);

    auto *kortholt = new(std::nothrow) Kortholt(std::move(cpuIds), stream, inputDeviceId, outputDeviceId);
    jlong handle = reinterpret_cast<jlong>(kortholt);

    LOGD("nativeCreateKortholt: created kortholt handle=%lld", (long long)handle);
    return handle;
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeDeleteKortholt(
        JNIEnv * /*unused*/,
        jobject /*unused*/,
        jlong kortholtHandle
) {
    LOGD("nativeDeleteKortholt: handle=%lld", (long long)kortholtHandle);
    delete reinterpret_cast<Kortholt *>(kortholtHandle);
    LOGD("nativeDeleteKortholt: deleted");
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeSetDefaultStreamValues(
        JNIEnv * /*unused*/,
        jobject /*unused*/,
        jint sampleRate,
        jint framesPerBurst
) {
    LOGD("nativeSetDefaultStreamValues: sampleRate=%d, framesPerBurst=%d", sampleRate, framesPerBurst);
    oboe::DefaultStreamValues::SampleRate = (int32_t) sampleRate;
    oboe::DefaultStreamValues::FramesPerBurst = (int32_t) framesPerBurst;
}

JNIEXPORT jint JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeSaveWaveFile(
        JNIEnv *env,
        jobject /*unused*/,
        jlong kortholtHandle,
        jstring fileName,
        jlong duration,
        jstring startBang,
        jstring stopBang
) {
    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        const char *name = env->GetStringUTFChars(fileName, nullptr);
        const char *start = env->GetStringUTFChars(startBang, nullptr);
        const char *stop = env->GetStringUTFChars(stopBang, nullptr);
        jint result = kortholt->saveWaveFile(name, duration, start, stop);
        env->ReleaseStringUTFChars(fileName, name);
        env->ReleaseStringUTFChars(startBang, start);
        env->ReleaseStringUTFChars(stopBang, stop);
        return result;
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeSetRecorderCallback(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle,
    jlong recorderHandle
) {
    LOGD("nativeSetRecorderCallback: kortholtHandle=%lld, recorderHandle=%lld",
         (long long)kortholtHandle, (long long)recorderHandle);

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    auto *recorder = reinterpret_cast<OboeAudioRecorderNative *>(recorderHandle);

    if (kortholt != nullptr && recorder != nullptr) {
        kortholt->setRecorderCallback(recorder);
    } else {
        LOGE("nativeSetRecorderCallback: null kortholt or recorder");
    }
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeClearRecorderCallback(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle
) {
    LOGD("nativeClearRecorderCallback: kortholtHandle=%lld", (long long)kortholtHandle);

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        kortholt->clearRecorderCallback();
    } else {
        LOGE("nativeClearRecorderCallback: null kortholt");
    }
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeSetDeviceIds(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle,
    jint inputDeviceId,
    jint outputDeviceId
) {
    LOGD("nativeSetDeviceIds: kortholtHandle=%lld, inputDeviceId=%d, outputDeviceId=%d",
         (long long)kortholtHandle, inputDeviceId, outputDeviceId);

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        kortholt->setDeviceIds(inputDeviceId, outputDeviceId);
    } else {
        LOGE("nativeSetDeviceIds: null kortholt");
    }
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeStartStreams(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle
) {
    LOGD("nativeStartStreams: handle=%lld", (long long)kortholtHandle);

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        kortholt->startStreams();
    } else {
        LOGE("nativeStartStreams: null kortholt");
    }
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeEnableMicInput(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle
) {
    LOGD("nativeEnableMicInput: handle=%lld", (long long)kortholtHandle);

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        kortholt->enableMicInput();
    } else {
        LOGE("nativeEnableMicInput: null kortholt");
    }
}

JNIEXPORT jint JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeGetStreamSampleRate(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle
) {
    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        return kortholt->getStreamSampleRate();
    }
    LOGE("nativeGetStreamSampleRate: null kortholt");
    return 0;
}

JNIEXPORT jboolean JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeIsInputDigitalSilence(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle
) {
    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        return kortholt->isInputDigitalSilence();
    }
    LOGE("nativeIsInputDigitalSilence: null kortholt");
    return false;
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeReopenInputStream(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle,
    jint preset,
    jint audioApi
) {
    LOGD("nativeReopenInputStream: handle=%lld, preset=%d, audioApi=%d",
         (long long)kortholtHandle, preset, audioApi);

    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        kortholt->reopenInputStream(
            static_cast<oboe::InputPreset>(preset),
            static_cast<oboe::AudioApi>(audioApi));
    } else {
        LOGE("nativeReopenInputStream: null kortholt");
    }
}

JNIEXPORT void JNICALL
Java_net_simno_kortholt_KortholtPlayer_nativeResetInputSilenceDetection(
    JNIEnv * /*unused*/,
    jobject /*unused*/,
    jlong kortholtHandle
) {
    auto *kortholt = reinterpret_cast<Kortholt *>(kortholtHandle);
    if (kortholt != nullptr) {
        kortholt->resetInputSilenceDetection();
    } else {
        LOGE("nativeResetInputSilenceDetection: null kortholt");
    }
}

} // extern "C"
