#pragma once
#include <glad/glad.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>

namespace RGL {

namespace glcore {

class GLInitExpt : public std::runtime_error {
public:
  GLInitExpt() = delete;
  explicit GLInitExpt(const std::string& msg);
  ~GLInitExpt() = default;
};
class GLLogicError : public std::runtime_error {
public:
  GLLogicError() = delete;
  explicit GLLogicError(const std::string& msg);

  ~GLLogicError() = default;
};

class GLShaderError : public std::runtime_error {
public:
  GLShaderError() = delete;
  explicit GLShaderError(const std::string& msg);
  ~GLShaderError() = default;
};

constexpr std::string_view gl_error_string(GLenum const err) noexcept;

void glCheckError();

} // namespace glcore
} // namespace RGL
