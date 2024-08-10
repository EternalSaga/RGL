add_library(GLPractices)

target_sources(GLPractices PUBLIC FILE_SET CXX_MODULES FILES "DynamicEffect.ixx" "StaticEffect.ixx"  "ThreeDEffect.ixx")

target_compile_features(GLPractices PRIVATE cxx_std_23)

target_link_libraries(GLPractices PUBLIC GLFramework)
