module;
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <source_location>
#include <spdlog/common.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

export module rllogger;

namespace RGL {

export using LogLevel = spdlog::level::level_enum;
export inline constexpr LogLevel LogLevelTrace = spdlog::level::trace;
export inline constexpr LogLevel LogLevelDebug = spdlog::level::debug;
export inline constexpr LogLevel LogLevelInfo = spdlog::level::info;
export inline constexpr LogLevel LogLevelWarn = spdlog::level::warn;
export inline constexpr LogLevel LogLevelError = spdlog::level::err;
export inline constexpr LogLevel LogLevelCritical = spdlog::level::critical;

export struct SourceLocHasher {
	std::size_t operator()(const std::source_location& loc) const noexcept {
		std::size_t h1 =
			std::hash<std::string_view>{}(loc.file_name() ? loc.file_name() : "");
		std::size_t h2 = std::hash<int>{}(loc.line());
		std::size_t h3 = std::hash<std::string_view>{}(
			loc.function_name() ? loc.function_name() : "");
		std::size_t seed = 0;
		seed ^= h1 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= h2 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		seed ^= h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		return seed;
	}
};

export struct SourceLocEqual {
	bool operator()(const std::source_location& lhs,
		const std::source_location& rhs) const noexcept {
		return lhs.line() == rhs.line() &&
			   (lhs.file_name() == rhs.file_name() ||
				   (lhs.file_name() && rhs.file_name() &&
					   std::string_view(lhs.file_name()) ==
						   std::string_view(rhs.file_name()))) &&
			   (lhs.function_name() == rhs.function_name() ||
				   (lhs.function_name() && rhs.function_name() &&
					   std::string_view(lhs.function_name()) ==
						   std::string_view(rhs.function_name())));
	}
};

export [[nodiscard]] constexpr auto
get_log_source_location(const std::source_location& location) {
	return spdlog::source_loc{location.file_name(),
		static_cast<std::int32_t>(location.line()),
		location.function_name()};
}

export struct format_with_location {
	std::string_view value;
	spdlog::source_loc loc;
	std::source_location stdloc;

	template <typename String>
	format_with_location(const String& s, const std::source_location& location =
											  std::source_location::current())
		: value{s}, loc{get_log_source_location(location)}, stdloc{location} {}
};

export class RLLogger {
   public:
	~RLLogger();
	static void
	initialize(const std::string& configPath = "configs/log_level.json");
	static RLLogger* getInstance();

	RLLogger(const RLLogger&) = delete;
	RLLogger& operator=(const RLLogger&) = delete;

	template <typename... Args>
	void log(LogLevel lvl, format_with_location fmt_, Args&&... args) {
		singletonLogger->log(fmt_.loc, lvl, fmt::runtime(fmt_.value),
			std::forward<Args>(args)...);
	}

	template <typename... Args>
	void log_every_n(LogLevel lvl, size_t n, format_with_location fmt_,
		Args&&... args) {
		if (n == 0) {
			log(lvl, fmt_, std::forward<Args>(args)...);
			return;
		}
		std::scoped_lock<std::mutex> lo(logCountMutex);
		auto it = logCount.find(fmt_.stdloc);
		if (it == logCount.end()) {
			it = logCount.emplace(fmt_.stdloc, 0).first;
		}
		if (++(it->second) % n == 0) {
			log(lvl, fmt_, std::forward<Args>(args)...);
		}
	}

	template <typename... Args>
	void log_if(LogLevel lvl, const bool condition, format_with_location fmt_,
		Args&&... args) {
		if (condition) {
			log(lvl, fmt_, std::forward<Args>(args)...);
		}
	}

	template <typename... Args>
	void log_if_every_n(LogLevel lvl, const bool condition, size_t n,
		format_with_location fmt_, Args&&... args) {
		if (condition) {
			log_every_n(lvl, n, fmt_, std::forward<Args>(args)...);
		}
	}

	template <typename... Args>
	void trace(format_with_location fmt_, Args&&... args) {
		log(LogLevelTrace, fmt_, std::forward<Args>(args)...);
	}

	template <typename... Args>
	void debug(format_with_location fmt_, Args&&... args) {
		log(LogLevelDebug, fmt_, std::forward<Args>(args)...);
	}

	template <typename... Args>
	void info(format_with_location fmt_, Args&&... args) {
		log(LogLevelInfo, fmt_, std::forward<Args>(args)...);
	}

	template <typename... Args>
	void warn(format_with_location fmt_, Args&&... args) {
		log(LogLevelWarn, fmt_, std::forward<Args>(args)...);
	}

	template <typename... Args>
	void error(format_with_location fmt_, Args&&... args) {
		log(LogLevelError, fmt_, std::forward<Args>(args)...);
	}

	template <typename... Args>
	void critical(format_with_location fmt_, Args&&... args) {
		log(LogLevelCritical, fmt_, std::forward<Args>(args)...);
	}

   private:
	static std::once_flag initOnce;
	static std::unique_ptr<RLLogger> rllogger;
	std::unordered_map<std::source_location, std::size_t, SourceLocHasher,
		SourceLocEqual>
		logCount;
	std::mutex logCountMutex;

	spdlog::logger* singletonLogger;
	static std::string configFilePath;
	explicit RLLogger(const std::string& configPath);
};

std::once_flag RLLogger::initOnce{};
std::unique_ptr<RLLogger> RLLogger::rllogger = nullptr;
std::string RLLogger::configFilePath = "configs/log_level.json";

RLLogger::~RLLogger() {
	if (singletonLogger) {
		delete singletonLogger;
		singletonLogger = nullptr;
	}
}

RLLogger* RLLogger::getInstance() {
	std::call_once(initOnce,
		[]() { rllogger.reset(new RLLogger(configFilePath)); });
	return rllogger.get();
}

void RLLogger::initialize(const std::string& configPath) {
	configFilePath = configPath;
}

RLLogger::RLLogger(const std::string& configPath) {
	const std::filesystem::path logLevelConfigFile = configFilePath;
	if (!std::filesystem::exists(configFilePath)) {
		throw std::runtime_error("Log config file not found");
	}

	std::ifstream configFile(configFilePath);
	auto conf = nlohmann::json::parse(configFile);

	auto log_file_path = conf["log_file"];
	auto file_sink =
		std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file_path, true);
	file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");

	if (conf.contains("console_level")) {
		auto consoleLevelStr = conf["console_level"];
		spdlog::level::level_enum consoleLevel =
			spdlog::level::from_str(consoleLevelStr);
		console_sink->set_level(consoleLevel);
	} else {
		console_sink->set_level(spdlog::level::warn);
	}

	if (conf.contains("file_level")) {
		auto fileLevelStr = conf["file_level"];
		spdlog::level::level_enum fileLevel = spdlog::level::from_str(fileLevelStr);
		file_sink->set_level(fileLevel);
	} else {
		file_sink->set_level(spdlog::level::info);
	}

	singletonLogger = new spdlog::logger("gl_logger", {console_sink, file_sink});
	singletonLogger->set_level(spdlog::level::trace);
	singletonLogger->flush_on(spdlog::level::warn);
	singletonLogger->set_error_handler(
		[](const std::string& errorMsg) { throw std::runtime_error(errorMsg); });

	info(
		"Logger initialized. Configuration loaded from configs/log_level.json "
		"if available.");
	info("Console log level: {}",
		spdlog::level::to_string_view(singletonLogger->sinks()[0]->level()));
	info("File log level: {}",
		spdlog::level::to_string_view(singletonLogger->sinks()[1]->level()));
}

}  // namespace RGL
