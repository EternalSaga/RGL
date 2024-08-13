add_library(Abstractor)

target_sources(Abstractor PUBLIC FILE_SET CXX_MODULES FILES "api_types.ixx")

target_compile_features(Abstractor PRIVATE cxx_std_23)
