#include "Shader.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cassert>
#include "rllogger.hpp"
namespace RGL {
namespace glcore {

Shader::Shader(const ShaderSrcs &shaderSrcs) : compiled(0), linked(0) ,shader(0){
    logger = RGL::RLLogger::getInstance();


    shaderProgram = glcore::glCall(glCreateProgram);
    for (const auto &shaderType_Src : shaderSrcs) {
	std::vector<const char *> srcs;
	std::vector<std::string> srcs_strings;
	std::transform(shaderType_Src.second.begin(),
	    shaderType_Src.second.end(),
	    std::back_inserter(srcs_strings),
	    [this](const fs::path &p) { return loadFile(p); });

	std::transform(srcs_strings.begin(), srcs_strings.end(),
	    std::back_inserter(srcs),
	    [](const std::string &str) { return str.c_str(); });

	shader = glcore::glCall(
	    glCreateShader, static_cast<GLuint>(shaderType_Src.first));
	assert(glIsShader(shader));
	glcore::glCall(glShaderSource, shader, srcs.size(), srcs.data(),
	    nullptr);
	glcore::glCall(glCompileShader, shader);

	glcore::glCall(glGetShaderiv, shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
	    char infoLog[MaxShaderLogLength];
	    glGetShaderInfoLog(shader, MaxShaderLogLength, nullptr, infoLog);
	    logger->error(infoLog);
	    throw glcore::GLLogicError("The shader is compiled failed");
	}

	glcore::glCall(glAttachShader, shaderProgram, shader);
    }
    glcore::glCall(glLinkProgram, shaderProgram);

    glcore::glCall(glGetProgramiv, shaderProgram, GL_LINK_STATUS,
	&linked);
    if (!linked) {
	char infoLog[MaxShaderLogLength];
	glGetProgramInfoLog(shaderProgram, MaxShaderLogLength, nullptr,
	    infoLog);
	logger->error(infoLog);
	throw glcore::GLLogicError("The shader is linked failed");
    }

}
Shader::Shader(Shader &&other) noexcept {
    this->compiled = other.compiled;
    this->linked = other.linked;
    this->logger = other.logger;
    this->shaderProgram = other.shaderProgram;


    other.compiled = false;
    other.linked = false;
    other.logger = nullptr;
    other.shaderProgram = -1;
}
std::string
Shader::loadFile(const fs::path &p) {
    std::ifstream ifs(p, std::ios::in);

    if (ifs.fail()) {
	throw glcore::GLInitExpt("Open file failed");
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();
    return buffer.str();
}
void Shader::useProgram() {
    if (compiled && linked) {
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

	if (glIsShader(shader)) {
	    glCall(glDetachShader,shaderProgram, shader);
	    glCall(glDeleteShader, shader);
	}

    if (glIsProgram(shaderProgram)) {
	    glCall(glDeleteProgram,shaderProgram);
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
