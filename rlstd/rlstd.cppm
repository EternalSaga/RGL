// rlstd.cppm — Standard Library Module Interface for libstdc++
// libstdc++ does not yet support `import std`, so this module wraps
// all standard library headers used across the project and re-exports
// the commonly used names.

module;
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
export module rlstd;

export namespace std {
using std::array;
using std::asin;
using std::atan2;
using std::back_inserter;
using std::cerr;
using std::clamp;
using std::cos;
using std::cout;
using std::endl;
using std::error_code;
using std::expected;
using std::fstream;
using std::ifstream;
using std::logic_error;
using std::map;
using std::memcpy;
using std::move;
using std::numeric_limits;
using std::ofstream;
using std::optional;
using std::out_of_range;
using std::pair;
using std::runtime_error;
using std::set;
using std::shared_ptr;
using std::sin;
using std::source_location;
using std::string;
using std::string_view;
using std::strlen;
using std::to_string;
using std::unexpected;
using std::vector;

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
