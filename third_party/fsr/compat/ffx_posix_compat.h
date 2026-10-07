// RGL-PATCH(P3): MSVC 安全 CRT（*_s）在非-Windows 的最小替身。
//
// 为什么要有这个文件：ffx_vk.cpp / ffx_breadcrumbs_list.cpp 里 42 处调用
// wcscpy_s / wcstombs_s / sprintf_s / swprintf_s，全是 MSVC-only。AMD 的 VK 后端
// 只在 Windows 上编过，Linux 侧这些名字根本不存在。逐个改调用点会把 42 处 vendor
// 源码改成「我们自己的版本」，将来 diff 上游就废了；这里用一个 force-include 的
// 兼容头把差异收在一处（CMake 里只对 ffx_* target 生效，不污染引擎）。
//
// 语义对齐 MSVC：失败返回非 0 errno_t、成功返回 0；wcscpy_s 越界时把 dest 首字节
// 清 0（MSVC 的行为是缓冲区置空），避免调用方拿到半截字符串继续用。
//
// ⚠ 变参那几个是 `printf` 家族：加了 `__attribute__((format))`，让 -Wformat 继续
//   盯 vendor 里的格式串（否则这层包装会把格式检查一起吞掉）。
#pragma once

#if !defined(_WIN32)

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <cwchar>
#include <string.h>
#include <wchar.h>

// MSVC 的 _countof（vendor 在 ffx_fsr3upscaler.cpp 里当数组长度用）。模板版只对
// 真数组成立——退化成指针时编译失败，这正是我们要的（sizeof 版会静默给 8/sizeof）。
template <typename T, size_t N>
char (&_countof_helper(T (&)[N]))[N];
#define _countof(arr) (sizeof(_countof_helper(arr)))

#ifndef FFX_COMPAT_FORMAT_ATTR
#if defined(__GNUC__)
#define FFX_COMPAT_FORMAT_ATTR(STRIDX, FIRSTVAR) __attribute__((format(printf, STRIDX, FIRSTVAR)))
#else
#define FFX_COMPAT_FORMAT_ATTR(STRIDX, FIRSTVAR)
#endif
#endif

// errno_t wcscpy_s(wchar_t* dest, size_t destSzInWords, const wchar_t* src)
inline int wcscpy_s(wchar_t* dest, size_t destSzInWords, const wchar_t* src)
{
    if (dest == nullptr || src == nullptr || destSzInWords == 0) return EINVAL;
    if (wcslen(src) + 1 > destSzInWords)
    {
        dest[0] = L'\0';  // MSVC: 失败时把目标置空，不留半截
        return ERANGE;
    }
    wcscpy(dest, src);
    return 0;
}

// MSVC 还有个模板重载 wcscpy_s(wchar_t (&)[N], const wchar_t*)（ffx_fsr3upscaler.cpp
// 里 32 处走的是这个 2 参形态）。只对真数组成立——退化成指针时编译失败，正是我们要的。
template <size_t N>
inline int wcscpy_s(wchar_t (&dest)[N], const wchar_t* src) { return wcscpy_s(dest, N, src); }
template <size_t N>
inline int strcpy_s(char (&dest)[N], const char* src) { return strcpy_s(dest, N, src); }

// errno_t strcpy_s(char* dest, size_t destSzInBytes, const char* src)
inline int strcpy_s(char* dest, size_t destSzInBytes, const char* src)
{
    if (dest == nullptr || src == nullptr || destSzInBytes == 0) return EINVAL;
    if (strlen(src) + 1 > destSzInBytes)
    {
        dest[0] = '\0';
        return ERANGE;
    }
    strcpy(dest, src);
    return 0;
}

// errno_t wcstombs_s(size_t* pRet, char* dest, size_t destSz, const wchar_t* src, size_t count)
// 手写而不用 libc 的 wcstombs：后者在 glibc 上声明于 <stdlib.h> 且吃 locale，而
// Linux 的 wchar_t 是 UTF-32 —— 依赖「当前 locale 恰好是 UTF-8」是隐式契约。这里
// 直接做 UTF-32 → UTF-8，行为与 locale 无关（vendor 的调用点只喂 ASCII 调试标签，
// 但非 ASCII 也不该静默变成问号）。
inline int wcstombs_s(size_t* pRet, char* dest, size_t destSz, const wchar_t* src, size_t count)
{
    if (dest == nullptr || src == nullptr || destSz == 0) return EINVAL;
    size_t out = 0;
    while (*src != L'\0' && out < count && out + 1 < destSz)
    {
        const uint32_t cp = static_cast<uint32_t>(*src++);
        unsigned char  seq[4];
        size_t         n = 0;
        if (cp < 0x80u) { seq[0] = static_cast<unsigned char>(cp); n = 1; }
        else if (cp < 0x800u) { seq[0] = static_cast<unsigned char>(0xC0u | (cp >> 6)); seq[1] = static_cast<unsigned char>(0x80u | (cp & 0x3Fu)); n = 2; }
        else if (cp < 0x10000u) { seq[0] = static_cast<unsigned char>(0xE0u | (cp >> 12)); seq[1] = static_cast<unsigned char>(0x80u | ((cp >> 6) & 0x3Fu)); seq[2] = static_cast<unsigned char>(0x80u | (cp & 0x3Fu)); n = 3; }
        else { seq[0] = static_cast<unsigned char>(0xF0u | (cp >> 18)); seq[1] = static_cast<unsigned char>(0x80u | ((cp >> 12) & 0x3Fu)); seq[2] = static_cast<unsigned char>(0x80u | ((cp >> 6) & 0x3Fu)); seq[3] = static_cast<unsigned char>(0x80u | (cp & 0x3Fu)); n = 4; }
        if (out + n >= destSz) break;  // 留不出 NUL 就整条不写
        for (size_t k = 0; k < n; ++k) dest[out++] = static_cast<char>(seq[k]);
    }
    dest[out] = '\0';
    if (pRet) *pRet = out;
    return 0;
}

// errno_t sprintf_s(char* buf, size_t bufSz, const char* fmt, ...)
inline int sprintf_s(char* buffer, size_t sizeOfBuffer, const char* format, ...) FFX_COMPAT_FORMAT_ATTR(3, 4);
inline int sprintf_s(char* buffer, size_t sizeOfBuffer, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    const int n = vsnprintf(buffer, sizeOfBuffer, format, ap);
    va_end(ap);
    if (n < 0 || static_cast<size_t>(n) >= sizeOfBuffer)
    {
        if (sizeOfBuffer) buffer[0] = '\0';
        return -1;  // MSVC: 截断即失败（调用方据此出声，不要静默截断）
    }
    return n;
}

// int swprintf_s(wchar_t* buf, size_t bufSz, const wchar_t* fmt, ...)
inline int swprintf_s(wchar_t* buffer, size_t sizeOfBuffer, const wchar_t* format, ...);
inline int swprintf_s(wchar_t* buffer, size_t sizeOfBuffer, const wchar_t* format, ...)
{
    va_list ap;
    va_start(ap, format);
    const int n = vswprintf(buffer, sizeOfBuffer, format, ap);
    va_end(ap);
    if (n < 0 || static_cast<size_t>(n) >= sizeOfBuffer)
    {
        if (sizeOfBuffer) buffer[0] = L'\0';
        return -1;
    }
    return n;
}

#endif  // !_WIN32
