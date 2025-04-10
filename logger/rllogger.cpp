#include "rllogger.hpp"
#include <fmt/format.h>
#include <spdlog/common.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <fstream>

using json = nlohmann::json;

namespace RGL {

RLLogger::~RLLogger() {
    if (singletonLogger) {
	delete singletonLogger;
	singletonLogger = nullptr;
    }
}
RLLogger* RLLogger::getInstance() {
    std::call_once(initOnce, []() {
		rllogger.reset(new RLLogger());
    });

   return rllogger.get();
}

std::once_flag RLLogger::initOnce{};
std::unique_ptr<RLLogger> RLLogger::rllogger = nullptr;
RLLogger::RLLogger()
{

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");

	

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
	"logs/gllog.txt", true);
	
	file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
    const std::filesystem::path logLevelConfigFile = "logs/log_level.json";
    if (std::filesystem::exists(logLevelConfigFile)) {
	std::ifstream configFile(logLevelConfigFile);
	auto conf = json::parse(configFile);
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
	    spdlog::level::level_enum fileLevel =
		spdlog::level::from_str(fileLevelStr);
	    file_sink->set_level(fileLevel);

	} else {
	    file_sink->set_level(spdlog::level::info);
	}
    }

    singletonLogger =
	new spdlog::logger("gl_logger", {console_sink, file_sink});
    singletonLogger->set_level(spdlog::level::trace);
    singletonLogger->set_error_handler([](const std::string &errorMsg) {
	throw std::runtime_error(errorMsg);
    }
	);



    info("Logger initialized. Configuration loaded from logs/log_level.json if available.");
    info("Console log level: {}", spdlog::level::to_string_view(singletonLogger->sinks()[0]->level()));
    info("File log level: {}", spdlog::level::to_string_view(singletonLogger->sinks()[1]->level()));
}
std::size_t SourceLocHasher::operator()(const std::source_location& loc) const noexcept {

    std::size_t h1 = std::hash<std::string_view>{}(loc.file_name() ? loc.file_name() : "");
    std::size_t h2 = std::hash<int>{}(loc.line());
    std::size_t h3 = std::hash<std::string_view>{}(loc.function_name() ? loc.function_name() : "");

    std::size_t seed = 0;
    seed ^= h1 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= h2 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    seed ^= h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    return seed;

}
bool SourceLocEqual::operator()(const std::source_location& lhs, const std::source_location& rhs) const noexcept {
    
     return lhs.line() == rhs.line() &&
    (lhs.file_name() == rhs.file_name() || (lhs.file_name() && rhs.file_name() && std::string_view(lhs.file_name()) == std::string_view(rhs.file_name()))) &&
    (lhs.function_name() == rhs.function_name() || (lhs.function_name() && rhs.function_name() && std::string_view(lhs.function_name()) == std::string_view(rhs.function_name())));
}
}  // namespace RGL
