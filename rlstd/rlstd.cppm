// rlstd.cppm — Standard Library Module Interface for libstdc++
// libstdc++ does not yet support `import std`, so this module wraps
// all standard library headers used across the project and re-exports
// the commonly used names.

module;
#include <algorithm>
#include <array>
#include <cctype>
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
    using std::string;
    using std::string_view;
    using std::to_string;
    using std::vector;
    using std::map;
    using std::set;
    using std::pair;
    using std::move;
    using std::clamp;
    using std::runtime_error;
    using std::out_of_range;
    using std::logic_error;
    using std::numeric_limits;
    using std::back_inserter;
    using std::source_location;
    using std::memcpy;
    using std::strlen;
    using std::ifstream;
    using std::ofstream;
    using std::fstream;
    using std::cerr;
    using std::cout;
    using std::endl;
    using std::expected;
    using std::unexpected;
    using std::error_code;
    using std::shared_ptr;

    namespace ranges {
        using std::ranges::any_of;
        using std::ranges::copy_n;
        using std::ranges::find;
        using std::ranges::find_if;
        using std::ranges::for_each;
        using std::ranges::sort;
        using std::ranges::transform;
    }
}
