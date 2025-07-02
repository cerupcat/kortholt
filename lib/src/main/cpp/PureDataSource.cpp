#include "PureDataSource.h"
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <string>

// Declare external setup function
extern "C" void externals_setup(void);


#define LOG_TAG "PureDataSource"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// JNI bridge receiver that forwards messages from native PD to Java PD receiver
class JavaBridgedReceiver : public pd::PdReceiver {
private:
    JavaVM* jvm;
    jobject javaReceiver;
    
    // Cached method IDs for efficiency
    jmethodID receiveFloatMethod;
    jmethodID receiveListMethod;
    jmethodID receiveBangMethod;
    jmethodID receiveSymbolMethod;
    jmethodID receiveMessageMethod;
    jmethodID printMethod;
    
    JNIEnv* getEnvForCallback() {
        JNIEnv* env = nullptr;
        if (jvm && jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            return env;
        }
        return nullptr;
    }

public:
    JavaBridgedReceiver() : jvm(nullptr), javaReceiver(nullptr) {
        // Initialize method IDs to nullptr
        receiveFloatMethod = nullptr;
        receiveListMethod = nullptr;
        receiveBangMethod = nullptr;
        receiveSymbolMethod = nullptr;
        receiveMessageMethod = nullptr;
        printMethod = nullptr;
    }
    
    void setJavaReceiver(JavaVM* vm, jobject receiver) {
        jvm = vm;
        if (receiver) {
            JNIEnv* env = getEnvForCallback();
            if (env) {
                javaReceiver = env->NewGlobalRef(receiver);
                
                // Cache method IDs for efficient callbacks
                jclass receiverClass = env->GetObjectClass(javaReceiver);
                receiveFloatMethod = env->GetMethodID(receiverClass, "receiveFloat", "(Ljava/lang/String;F)V");
                receiveListMethod = env->GetMethodID(receiverClass, "receiveList", "(Ljava/lang/String;[Ljava/lang/Object;)V");
                receiveBangMethod = env->GetMethodID(receiverClass, "receiveBang", "(Ljava/lang/String;)V");
                receiveSymbolMethod = env->GetMethodID(receiverClass, "receiveSymbol", "(Ljava/lang/String;Ljava/lang/String;)V");
                receiveMessageMethod = env->GetMethodID(receiverClass, "receiveMessage", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/Object;)V");
                printMethod = env->GetMethodID(receiverClass, "print", "(Ljava/lang/String;)V");
                
                env->DeleteLocalRef(receiverClass);
                jvm->DetachCurrentThread();
                LOGD("JavaBridgedReceiver: Java receiver set up with method IDs cached");
            }
        }
    }
    
    void print(const std::string& message) override {
        LOGD("PD Print: %s", message.c_str());
        
        // Forward to Java receiver
        if (jvm && javaReceiver && printMethod) {
            JNIEnv* env = getEnvForCallback();
            if (env) {
                jstring jmessage = env->NewStringUTF(message.c_str());
                env->CallVoidMethod(javaReceiver, printMethod, jmessage);
                env->DeleteLocalRef(jmessage);
                jvm->DetachCurrentThread();
            }
        }
    }
    
    void receiveFloat(const std::string &dest, float num) override {
        LOGD("PD Native->Java: receiveFloat %s = %f", dest.c_str(), num);
        
        if (jvm && javaReceiver && receiveFloatMethod) {
            JNIEnv* env = getEnvForCallback();
            if (env) {
                jstring jdest = env->NewStringUTF(dest.c_str());
                env->CallVoidMethod(javaReceiver, receiveFloatMethod, jdest, num);
                env->DeleteLocalRef(jdest);
                jvm->DetachCurrentThread();
            }
        }
    }
    
    void receiveBang(const std::string &dest) override {
        LOGD("PD Native->Java: receiveBang %s", dest.c_str());
        
        if (jvm && javaReceiver && receiveBangMethod) {
            JNIEnv* env = getEnvForCallback();
            if (env) {
                jstring jdest = env->NewStringUTF(dest.c_str());
                env->CallVoidMethod(javaReceiver, receiveBangMethod, jdest);
                env->DeleteLocalRef(jdest);
                jvm->DetachCurrentThread();
            }
        }
    }
    
    void receiveSymbol(const std::string &dest, const std::string &symbol) override {
        LOGD("PD Native->Java: receiveSymbol %s = %s", dest.c_str(), symbol.c_str());
        
        if (jvm && javaReceiver && receiveSymbolMethod) {
            JNIEnv* env = getEnvForCallback();
            if (env) {
                jstring jdest = env->NewStringUTF(dest.c_str());
                jstring jsymbol = env->NewStringUTF(symbol.c_str());
                env->CallVoidMethod(javaReceiver, receiveSymbolMethod, jdest, jsymbol);
                env->DeleteLocalRef(jdest);
                env->DeleteLocalRef(jsymbol);
                jvm->DetachCurrentThread();
            }
        }
    }
    
    void receiveList(const std::string &dest, const pd::List& list) override {
        LOGD("PD Native->Java: receiveList %s with %u items", dest.c_str(), list.len());
        
        if (jvm && javaReceiver && receiveListMethod) {
            JNIEnv* env = getEnvForCallback();
            if (env) {
                // Convert pd::List to Java Object array
                jobjectArray jarray = env->NewObjectArray(list.len(), env->FindClass("java/lang/Object"), nullptr);
                
                for (size_t i = 0; i < list.len(); i++) {
                    if (list.isFloat(i)) {
                        jobject jfloat = env->NewObject(env->FindClass("java/lang/Float"), 
                                                       env->GetMethodID(env->FindClass("java/lang/Float"), "<init>", "(F)V"), 
                                                       list.getFloat(i));
                        env->SetObjectArrayElement(jarray, i, jfloat);
                        env->DeleteLocalRef(jfloat);
                    } else if (list.isSymbol(i)) {
                        jstring jsymbol = env->NewStringUTF(list.getSymbol(i).c_str());
                        env->SetObjectArrayElement(jarray, i, jsymbol);
                        env->DeleteLocalRef(jsymbol);
                    }
                }
                
                jstring jdest = env->NewStringUTF(dest.c_str());
                env->CallVoidMethod(javaReceiver, receiveListMethod, jdest, jarray);
                env->DeleteLocalRef(jdest);
                env->DeleteLocalRef(jarray);
                jvm->DetachCurrentThread();
            }
        }
    }
    
    void receiveMessage(const std::string &dest, const std::string &msg, const pd::List& list) override {
        LOGD("PD Native->Java: receiveMessage %s %s with %u args", dest.c_str(), msg.c_str(), list.len());
        
        if (jvm && javaReceiver && receiveMessageMethod) {
            JNIEnv* env = getEnvForCallback();
            if (env) {
                // Convert pd::List to Java Object array (same as receiveList)
                jobjectArray jarray = env->NewObjectArray(list.len(), env->FindClass("java/lang/Object"), nullptr);
                
                for (size_t i = 0; i < list.len(); i++) {
                    if (list.isFloat(i)) {
                        jobject jfloat = env->NewObject(env->FindClass("java/lang/Float"), 
                                                       env->GetMethodID(env->FindClass("java/lang/Float"), "<init>", "(F)V"), 
                                                       list.getFloat(i));
                        env->SetObjectArrayElement(jarray, i, jfloat);
                        env->DeleteLocalRef(jfloat);
                    } else if (list.isSymbol(i)) {
                        jstring jsymbol = env->NewStringUTF(list.getSymbol(i).c_str());
                        env->SetObjectArrayElement(jarray, i, jsymbol);
                        env->DeleteLocalRef(jsymbol);
                    }
                }
                
                jstring jdest = env->NewStringUTF(dest.c_str());
                jstring jmsg = env->NewStringUTF(msg.c_str());
                env->CallVoidMethod(javaReceiver, receiveMessageMethod, jdest, jmsg, jarray);
                env->DeleteLocalRef(jdest);
                env->DeleteLocalRef(jmsg);
                env->DeleteLocalRef(jarray);
                jvm->DetachCurrentThread();
            }
        }
    }
    
    ~JavaBridgedReceiver() {
        if (jvm && javaReceiver) {
            JNIEnv* env = getEnvForCallback();
            if (env) {
                env->DeleteGlobalRef(javaReceiver);
                jvm->DetachCurrentThread();
            }
        }
    }
};

PureDataSource::PureDataSource(int32_t ticksPerBuffer) {
    this->ticksPerBuffer = ticksPerBuffer;
    pdBase = std::make_shared<pd::PdBase>();
    
    // Set up Java bridged receiver to forward PD messages to Java
    bridgedReceiver = std::make_shared<JavaBridgedReceiver>();
    pdBase->setReceiver(bridgedReceiver.get());
    LOGD("PureDataSource created with Java bridged receiver");
}

void PureDataSource::init(int32_t sampleRate, int32_t channelCount) {
    LOGD("Initializing Pure Data: sampleRate=%d, channelCount=%d, ticksPerBuffer=%d", 
         sampleRate, channelCount, ticksPerBuffer);
    
    LOGD("About to call pdBase->init()...");
    bool initResult = pdBase->init(0, channelCount, sampleRate, false);
    LOGD("pdBase->init() returned: %s", initResult ? "true" : "false");
    
    if (initResult) {
        LOGD("Calling pdBase->computeAudio(true)...");
        pdBase->computeAudio(true);
        
        // Initialize externals
        LOGD("Initializing Pure Data externals...");
        externals_setup();
        LOGD("Pure Data externals initialized");
        
        LOGD("Pure Data initialized successfully");
        LOGD("Pure Data initialization complete - print receiver active");
        
        // Test the print receiver by sending a message to Pure Data
        LOGD("Testing print receiver...");
        pdBase->sendSymbol("pd", "version");
    } else {
        LOGE("Failed to initialize Pure Data");
    }
}

void PureDataSource::renderAudio(float *audioData, int32_t numFrames) {
    // Create proper input buffer for Pure Data - zero-filled for tone generation
    static float* inputBuffer = nullptr;
    static int32_t lastBufferSize = 0;
    static int debugCounter = 0;
    
    int32_t bufferSize = ticksPerBuffer * 64; // PD uses 64-sample blocks
    
    // Allocate input buffer if needed
    if (inputBuffer == nullptr || bufferSize != lastBufferSize) {
        if (inputBuffer != nullptr) {
            delete[] inputBuffer;
        }
        inputBuffer = new float[bufferSize]();  // Zero-initialized
        lastBufferSize = bufferSize;
        LOGD("Allocated input buffer: size=%d", bufferSize);
    }
    
    // Clear output buffer first
    memset(audioData, 0, numFrames * 2 * sizeof(float)); // stereo
    
    // Debug Pure Data state
    if (debugCounter % 240 == 0) { // Every ~5 seconds
        LOGD("PD Debug - ticksPerBuffer: %d, bufferSize: %d, numFrames: %d", 
             ticksPerBuffer, bufferSize, numFrames);
        LOGD("PD Debug - pdBase initialized: %s", pdBase ? "YES" : "NO");
    }
    
    // Process audio through Pure Data
    bool result = pdBase->processFloat(ticksPerBuffer, inputBuffer, audioData);
    
    // Debug: Check if we're getting any output
    if (debugCounter++ % 480 == 0) { // Log every ~10 seconds at 48kHz
        float maxSample = 0.0f;
        for (int i = 0; i < numFrames * 2; i++) { // stereo
            if (abs(audioData[i]) > maxSample) {
                maxSample = abs(audioData[i]);
            }
        }
        LOGD("processFloat result: %s, frames: %d, max sample: %.6f", 
             result ? "SUCCESS" : "FAILED", numFrames, maxSample);
             
        // Sample a few output values for debugging
        LOGD("Sample values: [0]=%.6f, [1]=%.6f, [2]=%.6f, [3]=%.6f", 
             audioData[0], audioData[1], audioData[2], audioData[3]);
    }
}

void PureDataSource::sendBang(const char *dest) {
    pdBase->sendBang(dest);
}

void PureDataSource::sendFloat(const char *dest, float value) {
    pdBase->sendFloat(dest, value);
}

void PureDataSource::sendSymbol(const char *dest, const char *symbol) {
    pdBase->sendSymbol(dest, symbol);
}

bool PureDataSource::openPatch(const char *patch, const char *path) {
    LOGD("openPatch: patch=%s, path=%s", patch, path);
    
    pd::Patch patchHandle = pdBase->openPatch(patch, path);
    bool success = patchHandle.isValid();
    
    LOGD("openPatch result: %s", success ? "SUCCESS" : "FAILED");
    if (success) {
        LOGD("Patch opened with dollarZero: %d", patchHandle.dollarZero());
    }
    
    return success;
}

void PureDataSource::addToSearchPath(const char *path) {
    LOGD("addToSearchPath: path=%s", path);
    pdBase->addToSearchPath(path);
}

void PureDataSource::setJavaReceiver(JavaVM* jvm, jobject receiver) {
    LOGD("Setting up Java receiver bridge");
    if (bridgedReceiver) {
        auto javaBridge = std::static_pointer_cast<JavaBridgedReceiver>(bridgedReceiver);
        javaBridge->setJavaReceiver(jvm, receiver);
        LOGD("Java receiver bridge configured");
    } else {
        LOGE("No bridged receiver available");
    }
}


