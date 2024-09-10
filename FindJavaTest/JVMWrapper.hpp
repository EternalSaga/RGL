#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <iostream>
#include <type_traits>
#include <jni.h>

namespace RGL {
namespace vm {

class JVM {
    JavaVM *jvm;
    JNIEnv *env;
    JavaVMInitArgs vmArgs;
    std::unique_ptr<JavaVMOption[]> options;
    JVM(std::unique_ptr<JavaVMOption[]>, int size);

    static std::once_flag initFlag;
    static JVM *instance;

   public:
    static JVM *getInstance(std::unique_ptr<JavaVMOption[]>, int size);
    static JVM *getInstance();

    template<typename... Args>
    jobject createObject(const char *className, const char *signature,Args... args){
        jclass cls = env->FindClass(className);
        if (cls == nullptr) {
            throw std::runtime_error("Unable to find class");
        }
        jmethodID mid = env->GetMethodID(cls, "<init>", signature);
        jobject obj = env->NewObject(cls, mid, args...);
        return obj;
    }

    template<typename ReturnValue, typename... Args>
    ReturnValue callMethod(jobject obj, const char *methodName, const char *signature, Args... args){
        jclass cls = env->GetObjectClass(obj);
        jmethodID mid = env->GetMethodID(cls, methodName, signature);

        if (mid == nullptr) {
            throw std::runtime_error("Unable to find method");
        }

        if constexpr (std::is_same_v<ReturnValue, void>) {
            env->CallVoidMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jobject>) {
            return env->CallObjectMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jboolean>) {
            return env->CallBooleanMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jbyte>) {
            return env->CallByteMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jchar>) {
            return env->CallCharMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jshort>) {
            return env->CallShortMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jint>) {
            return env->CallIntMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jlong>) {
            return env->CallLongMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jfloat>) {
            return env->CallFloatMethod(obj, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jdouble>) {
            return env->CallDoubleMethod(obj, mid, std::forward<Args>(args)...);
        }else {
            throw std::runtime_error("Unsupported return type");
        }
    }


    template<typename ReturnValue, typename... Args>
    ReturnValue callStaticMethod(const char *className, const char *methodName, const char *signature, Args... args){
        jclass cls = env->FindClass(className);
        if (cls == nullptr) {
            throw std::runtime_error("Unable to find class");
        }
        jmethodID mid = env->GetStaticMethodID(cls, methodName, signature);

        if (mid == nullptr) {
            throw std::runtime_error("Unable to find method");
        }

        if constexpr (std::is_same_v<ReturnValue, void>) {

            env->CallStaticVoidMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jobject>) {
            return env->CallStaticObjectMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jboolean>) {
            return env->CallStaticBooleanMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jbyte>) {
            return env->CallStaticByteMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jchar>) {
            return env->CallStaticCharMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jshort>) {
            return env->CallStaticShortMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jshort>) {
            return env->CallStaticShortMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jint>) {
            return env->CallStaticIntMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jlong>) {
            return env->CallStaticLongMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jfloat>) {
            return env->CallStaticFloatMethod(cls, mid, std::forward<Args>(args)...);
        }
        else if constexpr (std::is_same_v<ReturnValue, jdouble>) {
            return env->CallStaticDoubleMethod(cls, mid, std::forward<Args>(args)...);
        }
        else {
            throw std::runtime_error("Unsupported return type");
        }
    }

    ~JVM();
};
}  // namespace vm

}  // namespace RGL