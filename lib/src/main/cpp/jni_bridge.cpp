#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include "Kortholt.h"

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
        jboolean stream
) {
    std::vector<int> cpuIds = convertJavaArrayToVector(env, jCpuIds);
    LOGD("nativeCreateKortholt: stream=%s, cpuIds.size=%zu", stream ? "true" : "false", cpuIds.size());
    
    auto *kortholt = new(std::nothrow) Kortholt(std::move(cpuIds), stream);
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


} // extern "C"
