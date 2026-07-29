set(VCPKG_TARGET_ARCHITECTURE x64 CACHE STRING "")
set(VCPKG_CRT_LINKAGE dynamic CACHE STRING "")
set(VCPKG_LIBRARY_LINKAGE static CACHE STRING "")
set(VCPKG_CMAKE_SYSTEM_NAME Linux CACHE STRING "")

# 优化标志（GCC 不需要 -stdlib）
set(VCPKG_C_FLAGS "-march=native -O2 -pipe" CACHE STRING "")
set(VCPKG_CXX_FLAGS "-march=native -O2 -pipe" CACHE STRING "")

# 补全 Release 的 -DNDEBUG
set(VCPKG_C_FLAGS_RELEASE "-march=native -O2 -pipe -DNDEBUG" CACHE STRING "")
set(VCPKG_CXX_FLAGS_RELEASE "-march=native -O2 -pipe -DNDEBUG" CACHE STRING "")
set(VCPKG_C_FLAGS_DEBUG "-march=native -g -pipe" CACHE STRING "")
set(VCPKG_CXX_FLAGS_DEBUG "-march=native -g -pipe" CACHE STRING "")

# 关键：显式告诉链接器带上 C++ 标准库
set(VCPKG_LINKER_FLAGS "-lstdc++" CACHE STRING "")
set(VCPKG_SHARED_LINKER_FLAGS "-lstdc++" CACHE STRING "")
