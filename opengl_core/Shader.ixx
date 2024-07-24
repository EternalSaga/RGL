module;
#include <glad/glad.h>
#include <filesystem>
#include <map>
#include <fstream>
#include <spdlog/logger.h>
export module Shader;

import GLCheckError;

namespace RGL {
	namespace io {
		namespace fs = std::filesystem;

		/*
		glCreateProgram->创建程序容器
		|
		v
		glCompileShader->编译着色器源码
		|
		+----->附加着色器到程序容器(glAttachShader)
		|
		|
		glLinkProgram->链接着色器
		|
		|
		v
		glUseProgram->激活程序容器
		*/


		/// <summary>
		/// 类型安全的Opengl shader类型，可以使用static_cast和宏定义转换
		/// </summary>
		export enum class SHADER_TYPE
		{
			VERTEX = GL_VERTEX_SHADER,
			FRAGMENT =  GL_FRAGMENT_SHADER,
			GEOMETRY = GL_GEOMETRY_SHADER,
			COMPUTE = GL_COMPUTE_SHADER,
			TESS_CONTROL = GL_TESS_CONTROL_SHADER,
			TESS_EVALUATION = GL_TESS_EVALUATION_SHADER
		};

		export using ShaderSrcs = std::map<SHADER_TYPE, std::vector<fs::path>>;

		export class Shader
		{

			spdlog::logger* logger;

		public:
			Shader() = delete;

			/// <summary>
			/// 读取shader类型和所有shader的文本文件，再编译
			/// </summary>
			/// <param name="shaderSrcs">
			/// 由SHADER_TYPE枚举和路径vector组成的map类型
			/// </param>
			Shader(const ShaderSrcs& shaderSrcs) : compiled(0)
				, linked(0) {

				logger = glcore::Logger::getInstance();

				// std::map<int, std::vector<fs::path>> -> 
				// std::map<int, std::vector<std::string> -> //be carefull for this 
				// std::map<int, std::vector<char*>> -> use glShaderSource 
				shaderProgram = glCreateProgram();
				for (const auto& shaderType_Src : shaderSrcs)
				{
					std::vector<const char*> srcs;
					std::vector<std::string> srcs_strings;
					std::transform(shaderType_Src.second.begin(), shaderType_Src.second.end(), std::back_inserter(srcs_strings), [this](const fs::path& p) {
						return loadFile(p);
						});

					std::transform(srcs_strings.begin(), srcs_strings.end(), std::back_inserter(srcs), [](const std::string& str) {return str.c_str(); });

					GLuint shader = glCreateShader(static_cast<GLuint>(shaderType_Src.first));
					glShaderSource(shader, srcs.size(), srcs.data(), nullptr);
					glCompileShader(shader);


					glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
					if (!compiled)
					{
						constexpr GLuint logLength = 1024;
						char infoLog[logLength];
						glGetShaderInfoLog(shader, logLength, nullptr, infoLog);
						logger->error(infoLog);
						throw glcore::GLLogicError("The shader is compiled failed");
					}

					glAttachShader(shaderProgram, shader);

				}
				glLinkProgram(shaderProgram);


				glGetProgramiv(shaderProgram, GL_LINK_STATUS, &linked);
				if (!linked)
				{
					constexpr GLuint logLength = 1024;
					char infoLog[logLength];
					glGetProgramInfoLog(shaderProgram, logLength, nullptr, infoLog);
					logger->error(infoLog);
					throw glcore::GLLogicError("The shader is linked failed");
				}
			}
			GLuint getShaderProgram() const {
				return shaderProgram;
			}

			operator GLuint() {
				return shaderProgram;
			}

			void useProgram() {
				if (compiled && linked)
				{
					glUseProgram(shaderProgram);
				}
				else {
					throw glcore::GLInitExpt("shader program not prepared");
				}
			}

			~Shader() {
				glUseProgram(0);
			}


			void endUse() {
				glUseProgram(0);
			}
		private:
			std::string loadFile(const fs::path& p){
				std::ifstream ifs(p, std::ios::in);

				if (ifs.fail())
				{
					throw glcore::GLInitExpt("Open file failed");
				}

				std::stringstream buffer;
				buffer << ifs.rdbuf();
				return buffer.str();
			}
			GLuint shaderProgram;

			GLint compiled;
			GLint linked;
		};

	}
}
