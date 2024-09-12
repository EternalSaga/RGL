#include "Shader.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cassert>
namespace RGL {
namespace glcore {

Shader::Shader(const ShaderSrcs &shaderSrcs) : compiled(0), linked(0) {
    logger = RGL::RLLogger::getInstance();

    // std::map<int, std::vector<fs::path>> ->
    // std::map<int, std::vector<std::string> -> //be carefull for this
    // std::map<int, std::vector<char*>> -> use glShaderSource
    shaderProgram = glcore::glCallRet(glCreateProgram);
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

	GLuint shader = glcore::glCallRet(
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
}  // namespace glcore
}  // namespace RGL
