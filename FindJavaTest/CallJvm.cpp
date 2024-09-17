
#include <memory>
#include <string>
#include <iostream>
#include "JVMWrapper.hpp"
int main(int argc, char* argv[]){

    std::unique_ptr<JavaVMOption[]> options = std::make_unique<JavaVMOption[]>(1);
    std::string path = "-Djava.class.path=build/FindJavaTest/HelloWorldJar.jar";
    options[0].optionString = path.data();
    RGL::vm::JVM* jvm = RGL::vm::JVM::getInstance(std::move(options), 1);
    jvm->callStaticMethod<void>("FindJavaTest/JavaMain", "main", "([Ljava/lang/String;)V",nullptr);
    jobject obj = jvm->createObject("FindJavaTest/JavaMain", "()V");
    int result = jvm->callMethod<jint>(obj, "javaAdd", "(II)I", 1, 2);
    std::cout << "Java Add Result: " << result << std::endl;    

    return 0;
}
