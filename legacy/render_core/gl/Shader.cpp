#include "Shader.hpp"
#include <glad/glad.h>
#include <cstddef>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cassert>
#include <vector>
#include "Helpers.hpp"
#include "rllogger.hpp"
namespace RGL {
namespace glcore {
std::string
loadFileStr(const fs::path &p) {
    std::ifstream ifs(p, std::ios::in);

    if (ifs.fail()) {
	throw glcore::GLInitExpt("Open file failed");
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();
    return buffer.str();
}

std::vector<char> loadFileBytes(const fs::path &p) {
    std::ifstream file(p, std::ios::binary | std::ios::ate);
    if (file.fail()) {
	RLLogger::getInstance()->critical("Failed to open file: {}", p.string());
	throw std::runtime_error("Failed to open file");
    }

    size_t size = file.tellg();
    std::vector<char> buffer(size);

    file.seekg(0);

    file.read(buffer.data(), size);
    if (buffer.empty()) {
	RLLogger::getInstance()->error("Failed to load shader file or file is empty: {}", p.string());
	// 这里应该有错误处理，比如直接抛出异常，防止后续操作失败
	throw std::runtime_error("Failed to read file");
    }
    return buffer;
}

Shader::Shader(const ShaderBytesPath &shaderBytesPath) {
    logger = RGL::RLLogger::getInstance();
    this->shaderProgram = glCall(glCreateProgram);

    std::map<SHADER_TYPE, GLuint> shaderObjects;

    for (auto [shaderType, shaderPath] : shaderBytesPath) {
	auto shaderBytes = loadFileBytes(shaderPath);

	GLuint shader;
	switch (shaderType) {
	case SHADER_TYPE::VERTEX:
	    shader = glCreateShader(GL_VERTEX_SHADER);
	    break;
	case SHADER_TYPE::FRAGMENT:
	    shader = glCreateShader(GL_FRAGMENT_SHADER);
	    break;
	case SHADER_TYPE::GEOMETRY:
	    shader = glCreateShader(GL_GEOMETRY_SHADER);
	    break;
	case SHADER_TYPE::COMPUTE:
	    shader = glCreateShader(GL_COMPUTE_SHADER);
	    break;
	case SHADER_TYPE::TESS_CONTROL:
	    shader = glCreateShader(GL_TESS_CONTROL_SHADER);
	    break;
	case SHADER_TYPE::TESS_EVALUATION:
	    shader = glCreateShader(GL_TESS_EVALUATION_SHADER);
	    break;

	default:
	    throw std::runtime_error("Unknown shader type");
	}
	shaderObjects[shaderType] = shader;

	glCall(glShaderBinary, 1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, shaderBytes.data(), shaderBytes.size());

	glCall(glSpecializeShader, shader, "main", 0, nullptr, nullptr);
	GLint success;
	glCall(glGetShaderiv, shader, GL_COMPILE_STATUS, &success);
	if (!success) {
	    char infoLog[MaxShaderLogLength];
	    glCall(glGetShaderInfoLog, shader, 1024, nullptr, infoLog);
	    logger->critical("Failed to specialize SPIR-V shader {}", infoLog);
	    // 清理已创建的对象
	    glCall(glDeleteShader, shader);
	    glCall(glDeleteProgram, this->shaderProgram);
	    throw std::runtime_error(infoLog);
	}
	glCall(glAttachShader, this->shaderProgram, shader);
    }
    glCall(glLinkProgram, this->shaderProgram);

    GLint success;
    glCall(glGetProgramiv, this->shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
	char infoLog[MaxShaderLogLength];
	glCall(glGetProgramInfoLog, this->shaderProgram, MaxShaderLogLength, nullptr, infoLog);
	logger->critical("Failed to link shader program: {}", infoLog);
	throw std::runtime_error(infoLog);
    }
    for (auto shaderObj : shaderObjects) {
	if (glIsShader(shaderObj.second)) {
	    glCall(glDetachShader, shaderProgram, shaderObj.second);
	    glCall(glDeleteShader, shaderObj.second);
	}
    }
    linked = 1;
}
Shader::Shader(Shader &&other) noexcept {
    this->logger = other.logger;
    this->shaderProgram = other.shaderProgram;

    other.logger = nullptr;
    other.shaderProgram = -1;
}

void Shader::useProgram() {
    if (linked) {
	assert(glIsProgram(shaderProgram));
	glcore::glCall(glUseProgram, shaderProgram);
    } else {
	throw glcore::GLInitExpt("shader program not prepared");
    }
}
void Shader::disableProgram() {
    glCall(glUseProgram, 0);
}
Shader::~Shader() {
    glCall(glUseProgram, 0);

    if (glIsProgram(shaderProgram)) {
	glCall(glDeleteProgram, shaderProgram);
    }
}
ScopeShader::ScopeShader(Shader &shader) : shader(shader) {
    shader.useProgram();
}
ScopeShader::~ScopeShader() {
    shader.disableProgram();
}

}  // namespace glcore
}  // namespace RGL
