#pragma once
#include <cstddef>
#include <memory>
#include <mutex>
#include <fmt/format.h>
#include <spdlog/common.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <nlohmann/detail/abi_macros.hpp>  // Note: This might not be strictly necessary here, but keep if other parts depend on it.
#include <fmt/format.h>			   // Note: Included twice, remove one.
#include <source_location>
#include <string_view>	// Include string_view explicitly
#include <unordered_map>

namespace RGL {

struct SourceLocHasher {
    std::size_t operator()(const std::source_location &loc) const noexcept;
};
struct SourceLocEqual {
    bool operator()(const std::source_location &lhs, const std::source_location &rhs) const noexcept;
};

[[nodiscard]] constexpr auto
get_log_source_location(const std::source_location &location) {
    return spdlog::source_loc{location.file_name(),
	static_cast<std::int32_t>(location.line()),
	location.function_name()};
}

struct format_with_location {
    std::string_view value;
    spdlog::source_loc loc;
    std::source_location stdloc;

    template <typename String>
    format_with_location(const String &s, const std::source_location &location =
					      std::source_location::current())
	: value{s}, loc{get_log_source_location(location)}, stdloc{location} {}
};

class RLLogger {
   public:
    ~RLLogger();
    static RLLogger *getInstance();
    RLLogger(const RLLogger &) = delete;
    RLLogger &operator=(const RLLogger &) = delete;

    template <typename... Args>
    void log(const spdlog::level::level_enum lvl, format_with_location fmt_, Args &&...args) {
	// Use fmt_.loc and fmt_.value provided by the implicitly constructed object
	singletonLogger->log(fmt_.loc, lvl, fmt::runtime(fmt_.value), std::forward<Args>(args)...);
    }

    template <typename... Args>
    void log_every_n(const spdlog::level::level_enum lvl, size_t n, format_with_location fmt_, Args &&...args) {
	if (n == 0) {  // Avoid division by zero and log every time if n is 0
	    log(lvl, fmt_, std::forward<Args>(args)...);
	    return;
	}

	std::scoped_lock<std::mutex> lo(logCountMutex);
	// Find or insert the counter for the specific source location
	auto it = logCount.find(fmt_.stdloc);
	if (it == logCount.end()) {
	    it = logCount.emplace(fmt_.stdloc, 0).first;
	}
	if (++(it->second) % n == 0) {
	    log(lvl, fmt_, std::forward<Args>(args)...);
	}
    }
    template <typename... Args>
    void log_if(const spdlog::level::level_enum lvl,const bool condition, format_with_location fmt_, Args &&...args) {
        if (condition) {
            log(lvl, fmt_, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void log_if_every_n(const spdlog::level::level_enum lvl, const bool condition, size_t n, format_with_location fmt_, Args &&...args) {
        if (condition) {
            log_every_n(lvl, n, fmt_, std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    void trace(format_with_location fmt_, Args &&...args) {
	log(spdlog::level::trace, fmt_, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(format_with_location fmt_, Args &&...args) {
	log(spdlog::level::debug, fmt_, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(format_with_location fmt_, Args &&...args) {
	log(spdlog::level::info, fmt_, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(format_with_location fmt_, Args &&...args) {
	log(spdlog::level::warn, fmt_, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(format_with_location fmt_, Args &&...args) {
	log(spdlog::level::err, fmt_, std::forward<Args>(args)...);
    }
    template <typename... Args>
    void critical(format_with_location fmt_, Args &&...args) {
	log(spdlog::level::critical, fmt_, std::forward<Args>(args)...);
    }

   private:
    static std::once_flag initOnce;
    static std::unique_ptr<RLLogger> rllogger;
    std::unordered_map<std::source_location, std::size_t, SourceLocHasher, SourceLocEqual> logCount;  // To implement log_every_n
    std::mutex logCountMutex;									      // Mutex to protect access to logCount map

    spdlog::logger *singletonLogger;

    RLLogger();
};

}  // namespace RGL