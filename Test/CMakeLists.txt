add_executable(TestGIL "TestGIL.cpp" )



find_package(Boost REQUIRED)

target_link_libraries(TestGIL PUBLIC Boost::headers)

find_package(JPEG REQUIRED)
target_link_libraries(TestGIL PRIVATE JPEG::JPEG)
find_package(PNG REQUIRED)
target_link_libraries(TestGIL PUBLIC PNG::PNG)

target_compile_features(TestGIL PRIVATE cxx_std_20)

