#pragma once
//
// sha1.h —— 输入 glb 的 SHA1（文件头缓存键，§7.8 拍板 3）。
// 工具侧 stdlib-only 纪律：不引 OpenSSL/xxhash 依赖，手写一份足够快的 SHA1
// （481MB 资产 ~1s，相对簇化 2.8s 与 glb 解析不是瓶颈）。
//
#include <cstdint>
#include <cstddef>
#include <string>

namespace RGL::meshpack {

// 流式接口：481MB 文件分块喂，不要求调用方常驻整份数据。
struct Sha1 {
    void init();
    void update(const void* data, size_t len);
    void finish(uint8_t out[20]);

    uint32_t state[5];
    uint64_t bitLen;
    uint8_t buf[64];
    size_t bufLen;
};

// 便捷：算一段内存的 SHA1。
void sha1Buffer(const void* data, size_t len, uint8_t out[20]);

// 小写十六进制（40 字符），打印用。
std::string sha1ToHex(const uint8_t digest[20]);

}  // namespace RGL::meshpack
