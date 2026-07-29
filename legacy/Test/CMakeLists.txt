add_executable(TestGIL "TestGIL.cpp" )

find_package(unofficial-duktape CONFIG REQUIRED)
target_link_libraries(TestGIL PRIVATE unofficial::duktape::duktape)

target_compile_features(TestGIL PRIVATE cxx_std_23)

find_path(DUKGLUE_INCLUDE_DIRS "dukglue/detail_class_proto.h")
target_include_directories(TestGIL PRIVATE ${DUKGLUE_INCLUDE_DIRS})
