#pragma once
#include "Helpers.hpp"
#include "glUniformWrapper.hpp"

#include <filesystem>
#include <fstream>
#include <glad/glad.h>

#include <map>


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
enum class SHADER_TYPE {
    VERTEX = GL_VERTEX_SHADER,
    FRAGMENT = GL_FRAGMENT_SHADER,
    GEOMETRY = GL_GEOMETRY_SHADER,
    COMPUTE = GL_COMPUTE_SHADER,
    TESS_CONTROL = GL_TESS_CONTROL_SHADER,
    TESS_EVALUATION = GL_TESS_EVALUATION_SHADER
};

using ShaderSrcs = std::map<SHADER_TYPE, std::vector<fs::path>>;

class Shader {
    spdlog::logger *logger;
    static constexpr GLuint MaxShaderLogLength = 1024;
    size_t *refCount = nullptr;

   public:
    Shader() = delete;

    // 方便map shader key 排序
    bool operator<(const Shader &other) const {
	return this->shaderProgram < other.shaderProgram;
    }

    /// <summary>
    /// 读取shader类型和所有shader的文本文件，再编译
    /// </summary>
    /// <param name="shaderSrcs">
    /// 由SHADER_TYPE枚举和路径vector组成的map类型
    /// </param>
    Shader(const ShaderSrcs &shaderSrcs);

    operator GLuint() { return shaderProgram; }
    operator GLuint() const { return shaderProgram; }
    void useProgram();

    void disableProgram();

    ~Shader();

    /// <summary>
    /// glUniformNumType的泛型封装
    /// </summary>
    /// <typeparam name="...Args">可变参数，多个变量的同一类型</typeparam>
    /// <param name="uniformVarName">shader里uniform变量名称</param>
    /// <param name="...values">需要传入shader的uniform变量数据</param>
    template <typename... Args>
    void setUniform(const std::string &uniformVarName, Args... values) {
	const auto location = glcore::glCallRet(
	    glGetUniformLocation, this->shaderProgram, uniformVarName.c_str());
	glcore::glUniform(shaderProgram, location, std::forward<Args>(values)...);
    }

    /// <summary>
    /// glUniformNumTypev的泛型封装
    /// Vector指的是uniform变量的数组，如果一个uniform变量是4个float，两个uniform变量就是8个float
    /// </summary>
    /// <typeparam name="T">float GLuint GLint三选一</typeparam>
    /// <typeparam name="UVarLength">unform变量的长度,1234选一</typeparam>
    /// <param name="uniformVarName">shader里uniform变量名称</param>
    /// <param name="data">需要传入shader的uniform变量数据</param>
    template <GLuint UVarLength, typename T>
    void setUniformVec(const std::string &uniformVarName, std::vector<T> data) {
	const auto location = glcore::glCall(
	    glGetUniformLocation, this->shaderProgram, uniformVarName.c_str());
	assert(data.size() % UVarLength ==
	       0);  // 数组长度=所有数据长度/uniform变量长度
	const GLsizei uniformVarCount = data.size() / UVarLength;

	glUniformVec(shaderProgram, location, uniformVarCount, data.data());
    }

    template <int Cols, int Rows, glm::qualifier q>
    void setUniform(const std::string &uniformName,
	const glm::mat<Cols, Rows, glm::f32, q> &m) {
	const auto location = glcore::glCall(
	    glGetUniformLocation, this->shaderProgram, uniformName.c_str());
	glUniform(shaderProgram, location, m);
    }

    Shader(Shader &&other) noexcept;

    Shader(const Shader &other) = delete;

   private:
    std::string loadFile(const fs::path &p);
    GLuint shaderProgram;

    GLint compiled;
    GLint linked;
};

class ScopeShader {
    Shader &shader;

   public:
    ScopeShader(Shader &shader);
    ~ScopeShader();
};



}  // namespace glcore
}  // namespace RGL
