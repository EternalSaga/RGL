#include "rllogger.hpp"

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
    console_sink->set_level(spdlog::level::warn);
    console_sink->set_pattern("[multi_sink_example] [%^%l%$] %v");

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        "logs/gllog.txt", true);
    file_sink->set_level(spdlog::level::info);

    singletonLogger =
        new spdlog::logger("gl_logger", {console_sink, file_sink});

    singletonLogger->set_level(spdlog::level::debug);
    singletonLogger->set_error_handler([](const std::string &errorMsg) {
      throw std::runtime_error(errorMsg);
    });
  });
  return singletonLogger;
}

std::once_flag RLLogger::initOnce{};
spdlog::logger *RLLogger::singletonLogger{nullptr};
} // namespace RGL
