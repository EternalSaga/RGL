add_executable(ECSTest "./ECSTest.cpp" )
target_compile_features(ECSTest PRIVATE cxx_std_23)

find_package(EnTT CONFIG REQUIRED)
target_link_libraries(ECSTest PRIVATE EnTT::EnTT)

add_executable(SignalTest "SignalTest.cpp")
target_link_libraries(SignalTest PRIVATE EnTT::EnTT)


add_executable(DelegateTest "DelegateTest.cpp")
target_link_libraries(DelegateTest PRIVATE EnTT::EnTT)
target_compile_features(DelegateTest PRIVATE cxx_std_23)
