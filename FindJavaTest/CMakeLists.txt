find_package(Java 21 REQUIRED)
message(STATUS "Java_JAVA_EXECUTABLE: ${Java_JAVA_EXECUTABLE}")

include(UseJava)
add_jar(HelloWorldJar JavaMain.java)

add_executable(CallJvm ./CallJvm.cpp ./JVMWrapper.cpp)


find_package(JNI 21 REQUIRED)
target_compile_features(CallJvm PRIVATE cxx_std_20)
target_include_directories(CallJvm PRIVATE ${JNI_INCLUDE_DIRS})
target_link_libraries(CallJvm PRIVATE ${JNI_LIBRARIES})

