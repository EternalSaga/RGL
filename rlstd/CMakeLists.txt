add_library(rlstd STATIC)
target_sources(rlstd PUBLIC
    FILE_SET CXX_MODULES
    FILES rlstd.cppm
)
target_compile_features(rlstd PUBLIC cxx_std_23)
