# /home/robin/program/RGL/custom-triplets/x64-linux-native.cmake

set(VCPKG_TARGET_ARCHITECTURE x64 CACHE STRING "")
set(VCPKG_CRT_LINKAGE dynamic CACHE STRING "")
set(VCPKG_LIBRARY_LINKAGE static CACHE STRING "") # 如果你想动态链接，这里改 dynamic
set(VCPKG_CMAKE_SYSTEM_NAME Linux CACHE STRING "")

# 注入你的 Gentoo 灵魂参数
set(VCPKG_C_FLAGS "-march=native -O2 -pipe" CACHE STRING "")
set(VCPKG_CXX_FLAGS "-march=native -O2 -pipe" CACHE STRING "")

# 强行覆盖 Release 和 Debug 的默认行为，防止它们被覆盖
set(VCPKG_C_FLAGS_RELEASE "-march=native -O2 -pipe" CACHE STRING "")
set(VCPKG_CXX_FLAGS_RELEASE "-march=native -O2 -pipe" CACHE STRING "")
set(VCPKG_C_FLAGS_DEBUG "-march=native -g -pipe" CACHE STRING "")
set(VCPKG_CXX_FLAGS_DEBUG "-march=native -g -pipe" CACHE STRING "")