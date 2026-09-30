// rlstd.cppm — Standard Library Module Interface for libstdc++
// libstdc++ does not yet support `import std`, so this module wraps
// all standard library headers used across the project and re-exports
// the commonly used names.

module;
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
export module rlstd;

// 全局作用域 C 整数名（stdint.h 系）：全库代码风格用裸 uint32_t/uint64_t 而非
// std:: 前缀；GMF 收敛后纯接口模块无 <cstdint> 传递来源，以本模块为唯一出口。
// （std::uint32_t 与 ::uint32_t 是同类型 typedef，与残留 GMF include 并存的
// 重声明合法。）
export using std::uint8_t;

export using std::uint16_t;
export using std::uint32_t;
export using std::uint64_t;
export using std::int32_t;
export using std::int64_t;
export using std::size_t;

// <cmath> 常用名补齐（2026-09-30，批 12.1 建 rlcore 时暴露）：sqrt/ceil/round/abs/
// acos/atan/cos/exp/log/tan/fabs 此前**从未导出**——rgl/scene_context.cppm 里的
// std::sqrt/std::ceil/std::round 是靠它自己 GMF 的 `#include <glm/glm.hpp>` 传递进来的
// <cmath> 才可见的（模块 GMF 的名字在本模块 purview 可见但**不导出**），所以
// 「rlstd 必须导出完整可用面」这条纪律在这些名字上一直是破的——只是全库 102 个 TU
// 都恰好 include 了 glm，没人写过一个不 include glm 却用它们的 TU。rlcore 的测试 TU
// 正是那种 TU，第一次把这个问题顶出来。
// 下方 std 命名空间块里已有的（floor/fmod/sin/pow/asin/atan2/max/min）不重复导出。
export namespace std {
using std::array;
using std::asin;
using std::atan2;
using std::back_inserter;
using std::cerr;
using std::clamp;
using std::condition_variable;
using std::copy_n;
using std::cos;
using std::cv_status;
using std::deque;
using std::cout;
using std::endl;
using std::fill;
using std::error_code;
using std::exception;
using std::FILE;
using std::fclose;
using std::fopen;
using std::fwrite;
using std::expected;
using std::as_bytes;
using std::byte;
using std::fstream;
using std::function;
using std::future;
using std::future_error;
using std::future_status;
using std::floor;
using std::fmod;
using std::ifstream;
using std::lock_guard;
using std::logic_error;
using std::map;
using std::max;
using std::memcpy;
using std::min;
using std::move;
using std::mutex;
using std::numeric_limits;
using std::ofstream;
using std::optional;
using std::out_of_range;
using std::pair;
using std::pow;
using std::promise;
using std::runtime_error;
using std::set;
using std::shared_ptr;
using std::sin;
using std::sqrt;   // 2026-09-30 补（批 12.1）：见下方 <cmath> 补齐说明
using std::ceil;
using std::round;
using std::abs;    // 整数重载来自 <cstdlib>，浮点重载来自 <cmath>，两者都在本模块 GMF
using std::fabs;
using std::acos;
using std::atan;
using std::cos;
using std::exp;
using std::log;
using std::tan;
using std::source_location;
using std::sort;
using std::span;
using std::stable_sort;
using std::string;
using std::string_view;
using std::strlen;
using std::strtoull;
using std::system;
using std::thread;
using std::to_string;
using std::tolower;
using std::toupper;
using std::unexpected;
using std::unordered_map;
using std::vector;

// 自由函数与运算符重载（GMF 已含全部相关头；using 一个运算符名导入整个重载集）：
// 2026-09-02 GMF 收敛后暴露的 rlstd 洞——这些名字原先全靠各 TU 的 vulkan.hpp/glm/fmt
// 传递 include 兜底；GMF 掏空后 std 容器/视图的跨类比较与迭代辅助必须显式导出
// （BMI 只带声明引用的类本体，不带 namespace 作用域的自由运算符）。
using std::advance;
using std::begin;
using std::distance;
using std::end;
using std::size;
using std::swap;
using std::operator&;
using std::operator+;
using std::operator|;
using std::operator<;
using std::operator<=;
using std::operator!=;
using std::operator==;
using std::operator>;
using std::operator>=;
using std::getenv;
using std::make_shared;
using std::make_tuple;
using std::memcmp;
using std::memcpy;
using std::memset;
using std::nullopt;
using std::optional;
using std::strcmp;
using std::tuple;
using std::make_unique;
using std::unique_lock;
using std::unique_ptr;
using std::ifstream;
using std::ios;
using std::ios_base;
using std::istream;
using std::ostream;
using std::ofstream;
}  // namespace std

// libstdc++ internals：std 容器迭代器实体（__normal_iterator）定义于 __gnu_cxx，
// 其比较运算符是 __gnu_cxx 作用域自由函数模板——不导出则 `it != c.end()` 无候选
// （C++20 重写机制：导出 == 与 <=>，!=/<=/> 等自动可用）。
export namespace __gnu_cxx {
using __gnu_cxx::operator==;
using __gnu_cxx::operator<=>;
}  // namespace __gnu_cxx


export namespace std {

namespace ranges {
using std::ranges::any_of;
using std::ranges::copy_n;
using std::ranges::find;
using std::ranges::find_if;
using std::ranges::for_each;
using std::ranges::sort;
using std::ranges::transform;
}  // namespace ranges
}  // namespace std
