
#pragma once
#include <mutex>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>


namespace RGL {

namespace lv = spdlog::level;
class RLLogger {
public:
  ~RLLogger();
  static spdlog::logger *getInstance();
  RLLogger(const RLLogger &) = delete;
  RLLogger &operator=(const RLLogger &) = delete;

private:
  static std::once_flag initOnce;
  // static std::unique_ptr<spdlog::lobgger> logger;
  static spdlog::logger *singletonLogger;

  RLLogger() = default;
};



} // namespace RGL
