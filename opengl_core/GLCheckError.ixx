module;
#include <glad/glad.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

export module GLCheckError;

namespace RGL {
	namespace glcore {
		namespace lv = spdlog::level;


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
					singletonLogger->set_error_handler([](const std::string& errorMsg) {
						throw std::runtime_error(errorMsg);
						});
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

		export void glCheckError() {
			GLenum errorCode = glGetError();
			std::string errStr;
			if (errorCode != GL_NO_ERROR)
			{
				switch (errorCode) {
				case GL_INVALID_ENUM:
					errStr = "GL_INVALID_ENUM";
					break;
				case GL_INVALID_OPERATION:
					errStr = "GL_INVALID_OPERATION";
					break;
				case GL_OUT_OF_MEMORY:
					errStr = "GL_OUT_OF_MEMORY";
					break;
				case GL_INVALID_VALUE:
					errStr = "GL_INVALID_VALUE";
					break;
				default:
					errStr = "UNKNOWN_ERROR";
					break;
				}
				//spdlog::log(lv::err, "OpenGL api error {}", errStr);
				auto logger = Logger::getInstance();
				logger->error("OpenGL api error {}", errStr);
			}
		}



	}
}

