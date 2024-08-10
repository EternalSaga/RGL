module;
#include <glad/glad.h>
#include <filesystem>
#include <map>
#include <fstream>
#include <spdlog/logger.h>
#include "Helpers.hpp"
#include "glUniformWrapper.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
export module Shader;

import GLCheckError;

namespace RGL {
	namespace glcore {
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
			static constexpr GLuint MaxShaderLogLength = 1024;
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
				shaderProgram = glcore::glCallRet(glCreateProgram);
				for (const auto& shaderType_Src : shaderSrcs)
				{
					std::vector<const char*> srcs;
					std::vector<std::string> srcs_strings;
					std::transform(shaderType_Src.second.begin(), shaderType_Src.second.end(), std::back_inserter(srcs_strings), [this](const fs::path& p) {
						return loadFile(p);
						});

					std::transform(srcs_strings.begin(), srcs_strings.end(), std::back_inserter(srcs), [](const std::string& str) {return str.c_str(); });

					GLuint shader = glcore::glCallRet(glCreateShader,static_cast<GLuint>(shaderType_Src.first));
					glcore::glCall(glShaderSource,shader, srcs.size(), srcs.data(), nullptr);
					glcore::glCall(glCompileShader,shader);


					glcore::glCall(glGetShaderiv,shader, GL_COMPILE_STATUS, &compiled);
					if (!compiled)
					{
						char infoLog[MaxShaderLogLength];
						glGetShaderInfoLog(shader, MaxShaderLogLength, nullptr, infoLog);
						logger->error(infoLog);
						throw glcore::GLLogicError("The shader is compiled failed");
					}

					glcore::glCall(glAttachShader,shaderProgram, shader);

				}
				glcore::glCall(glLinkProgram,shaderProgram);


				glcore::glCall(glGetProgramiv,shaderProgram, GL_LINK_STATUS, &linked);
				if (!linked)
				{
					char infoLog[MaxShaderLogLength];
					glGetProgramInfoLog(shaderProgram, MaxShaderLogLength, nullptr, infoLog);
					logger->error(infoLog);
					throw glcore::GLLogicError("The shader is linked failed");
				}
			}

			operator GLuint() {
				return shaderProgram;
			}

			void useProgram() {
				if (compiled && linked)
				{
					glcore::glCall(glUseProgram,shaderProgram);
				}
				else {
					throw glcore::GLInitExpt("shader program not prepared");
				}
			}

			~Shader() {
				glcore::glCall(glUseProgram,0);
			}


			/// <summary>
			/// glUniformNumType的泛型封装
			/// </summary>
			/// <typeparam name="...Args">可变参数，多个变量的同一类型</typeparam>
			/// <param name="uniformVarName">shader里uniform变量名称</param>
			/// <param name="...values">需要传入shader的uniform变量数据</param>
			template<typename... Args>
			void setUniform(const std::string& uniformVarName, Args... values){
				const auto location = glcore::glCallRet(glGetUniformLocation, this->shaderProgram, uniformVarName.c_str());
				glcore::glUniform(location, std::forward<Args>(values)...);
			}

			/// <summary>
			/// glUniformNumTypev的泛型封装
			/// Vector指的是uniform变量的数组，如果一个uniform变量是4个float，两个uniform变量就是8个float
			/// </summary>
			/// <typeparam name="T">float GLuint GLint三选一</typeparam>
			/// <typeparam name="UVarLength">unform变量的长度,1234选一</typeparam>
			/// <param name="uniformVarName">shader里uniform变量名称</param>
			/// <param name="data">需要传入shader的uniform变量数据</param>
			template<GLuint UVarLength,typename T>
			void setUniformVec(const std::string& uniformVarName,std::vector<T> data) {
				const auto location = glcore::glCallRet(glGetUniformLocation, this->shaderProgram, uniformVarName.c_str());
				assert(data.size() % UVarLength == 0);//数组长度=所有数据长度/uniform变量长度
				const GLsizei uniformVarCount = data.size() / UVarLength;
				
				glUniformVec(location, uniformVarCount, data.data());
			}


			template<GLuint Cols, GLuint Rows, glm::qualifier q>
			void setUniformMat(const std::string& uniformName,const glm::mat<Cols, Rows, float, q>& m) {
				const auto location = glcore::glCallRet(glGetUniformLocation, this->shaderProgram, uniformName.c_str());
				glUniformMatrix(location, m);
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
