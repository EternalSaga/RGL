#pragma once
#include <glad/glad.h>
#include "Helpers.hpp"
#include "GLObj.hpp"
#include "Shader.hpp"
#include "ShaderManager.hpp"
#include "rllogger.hpp"
#include <array>
#include <unordered_map>
namespace RGL {
namespace glcore {



std::string type2string(int gltype);
	
class UBO {
    GLuint ubo;

    GLint blockSize;
    GLuint blockIndex;
    GLint uniformCount;

	std::vector<GLint> uniformOffsets;
    std::vector<GLint> uniformTypes;
    std::vector<GLint> uniformArrayStrides;
    std::vector<GLint> uniformSizes;  // uniform 元素个数，用于数组
    std::vector<std::string> uniformNames;
    std::vector<std::byte> data;
    GLuint shader;
    std::string uboName;
    GLint bindingPoint;


    RLLogger* logger = nullptr;

	void prepareOffsetsAndDataType();

   public:
    UBO(GLuint shaderProgram, const std::string& uniformBlockName,GLint bindingPoint);


    void updateCpuUbo(const std::string& uboBlockName, const CommonUniformTypes& value);

    void setUniform();

	const std::string getUboName()const {
	return this->uboName;
	}

    ~UBO();
};
using UBOs = std::shared_ptr<std::unordered_map<std::string, std::shared_ptr<UBO>>>;
}  // namespace glcore
}  // namespace RGL
