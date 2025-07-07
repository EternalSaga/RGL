#pragma once
#include "Helpers.hpp"
#include "glUniformWrapper.hpp"
#include "rllogger.hpp"

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

using ShaderBytesPath = std::map<SHADER_TYPE, fs::path>;

class Shader {
    RLLogger *logger;
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
    Shader(const ShaderBytesPath &shaderBytesPath);

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
	const auto location = glcore::glCall(
	    glGetUniformLocation, this->shaderProgram, uniformVarName.c_str());
        if (location!=-1) {
            glcore::glUniform(shaderProgram, location, std::forward<Args>(values)...);
        }else {
            logger->error("uniform {} not found in shader", uniformVarName);
        }
	
    }



    Shader(Shader &&other) noexcept;

    Shader(const Shader &other) = delete;

   private:
    
    GLuint shaderProgram;
    GLint linked = 0;
    
};

class ScopeShader {
    Shader &shader;

   public:
    ScopeShader(Shader &shader);
    ~ScopeShader();
};



}  // namespace glcore
}  // namespace RGL
