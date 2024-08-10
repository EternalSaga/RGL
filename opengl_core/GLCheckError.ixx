module;
#include <glad/glad.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <string_view>
export module GLCheckError;

namespace RGL {
	namespace glcore {
		namespace lv = spdlog::level;
		
		export class GLInitExpt : public std::runtime_error
		{
		public:
			GLInitExpt() = delete;
			GLInitExpt(const std::string msg) :std::runtime_error(msg) {

			}
			~GLInitExpt() = default;
		};
		export class GLLogicError : public std::runtime_error
		{
		public:
			GLLogicError() = delete;
			GLLogicError(const std::string msg) :std::runtime_error(msg) {}

			~GLLogicError() = default;
		};

		export class GLShaderError : public std::runtime_error
		{
		public:
			GLShaderError() = delete;
			GLShaderError(const std::string msg) :std::runtime_error(msg) {}
			~GLShaderError() = default;
		};

		export class Logger
		{
		public:

			~Logger() {
				if (singletonLogger)
				{
					delete singletonLogger;
					singletonLogger = nullptr;
				}
			}
			static spdlog::logger* getInstance() {
				std::call_once(initOnce, []() {
					auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
					console_sink->set_level(spdlog::level::warn);
					console_sink->set_pattern("[multi_sink_example] [%^%l%$] %v");

					auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/gllog.txt", true);
					file_sink->set_level(spdlog::level::info);

					singletonLogger = new spdlog::logger("gl_logger", { console_sink,file_sink });

					singletonLogger->set_level(spdlog::level::debug);
					//singletonLogger->set_error_handler([](const std::string& errorMsg) {
					//	throw std::runtime_error(errorMsg);
					//	});
					
					});
				return singletonLogger;
			}
			Logger(const Logger&) = delete;
			Logger& operator=(const Logger&) = delete;
		private:
			static std::once_flag initOnce;
			//static std::unique_ptr<spdlog::logger> logger;
			static spdlog::logger* singletonLogger;

			Logger() = default;
		};

		std::once_flag Logger::initOnce{};
		spdlog::logger* Logger::singletonLogger{ nullptr };


		constexpr std::string_view gl_error_string(GLenum const err) noexcept
		{
			switch (err)
			{
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

		export void glCheckError() {
			GLenum errorCode = glGetError();
			
			if (errorCode != GL_NO_ERROR)
			{
				auto errorStr = gl_error_string(errorCode);
				//spdlog::log(lv::err, "OpenGL api error {}", errStr);
				auto logger = Logger::getInstance();
				logger->error("OpenGL api error {}", errorStr);


			}
		}

	}
}

