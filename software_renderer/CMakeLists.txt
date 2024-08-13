add_library(SoftRender)

target_sources(SoftRender PUBLIC FILE_SET CXX_MODULES FILES "SRFramework.ixx")


target_compile_features(SoftRender PUBLIC cxx_std_23)


target_link_libraries(SoftRender PRIVATE Abstractor RLApp)
