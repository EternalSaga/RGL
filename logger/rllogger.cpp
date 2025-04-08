#include "rllogger.hpp"
#include <fmt/format.h>
#include <spdlog/common.h>

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
spdlog::logger *RLLogger::getInstance() {
    std::call_once(initOnce, []() {
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

	console_sink->set_pattern("[multi_sink_example] [%^%l%$] %v");

	auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
	    "logs/gllog.txt", true);

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
	});

	singletonLogger->info("Logger initialized. Configuration loaded from logs/log_level.json if available.");
	singletonLogger->info("Console log level: {}", spdlog::level::to_string_view(singletonLogger->sinks()[0]->level()));
	singletonLogger->info("File log level: {}", spdlog::level::to_string_view(singletonLogger->sinks()[1]->level()));
	singletonLogger->debug("Debug message to check log level.");//won't output on terminal
	singletonLogger->trace("Trace message to check log level.");//won't output on terminal
    });

    return singletonLogger;
}

std::once_flag RLLogger::initOnce{};
spdlog::logger *RLLogger::singletonLogger{nullptr};
}  // namespace RGL
