#include "GLCheckError.hpp"
#include "rllogger.hpp"
namespace RGL {
    namespace glcore{

    GLInitExpt::GLInitExpt(const std::string& msg) : std::runtime_error(msg) {}
    GLLogicError::GLLogicError(const std::string& msg)
        : std::runtime_error(msg) {}
    void glCheckError() {
      GLenum errorCode = glGetError();

      if (errorCode != GL_NO_ERROR) {
        auto errorStr = gl_error_string(errorCode);

        auto logger = RGL::RLLogger::getInstance();
		
        logger->error("OpenGL api error {}", errorStr);
      }
    }
    constexpr std::string_view gl_error_string(GLenum const err) noexcept {
      switch (err) {
      case GL_NO_ERROR:
        return "GL_NO_ERROR";

      case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";

      case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";

      case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";

      case GL_STACK_OVERFLOW:
        return "GL_STACK_OVERFLOW";

      case GL_STACK_UNDERFLOW:
        return "GL_STACK_UNDERFLOW";

      case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";

      case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";

        // gles 2, 3 and gl 4 error are handled by the switch above
      default:
        return "unknown error";
      }
    }
    GLShaderError::GLShaderError(const std::string& msg)
        : std::runtime_error(msg) {}
    } // namespace glcore
}
