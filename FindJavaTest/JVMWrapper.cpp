#include "JVMWrapper.hpp"
namespace RGL{

namespace vm {

JVM *JVM::getInstance(std::unique_ptr<JavaVMOption[]> options, int size) {
    std::call_once(initFlag, [&]() {
	instance = new JVM(std::move(options), size);
    });
    return instance;
}
JVM *JVM::getInstance() {
    if (instance == nullptr) {
	throw std::runtime_error("JVM not initialized");
    }
    return instance;
}


std::once_flag JVM::initFlag{};
JVM* JVM::instance{nullptr};

JVM::~JVM() {
    jvm->DestroyJavaVM();
}
JVM::JVM(std::unique_ptr<JavaVMOption[]> options, int size) {
    vmArgs.version = JNI_VERSION_21;
    vmArgs.nOptions = size;
    vmArgs.options = options.get();
    vmArgs.ignoreUnrecognized = JNI_FALSE;
    jint res = JNI_CreateJavaVM(&jvm, (void **)&env, &vmArgs);
    if (res != JNI_OK) {
	throw std::runtime_error("Unable to launch JVM");
    }
}
}  // namespace vm
}