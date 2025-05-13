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

class UboBindingManager {
    static constexpr GLint MAX_BINDING_POINTS = 32;
    std::array<bool, MAX_BINDING_POINTS> usedBindingPoints;
    static std::unique_ptr<UboBindingManager> instance;
    static std::once_flag initOnce;
    UboBindingManager();

   public:
    
    static UboBindingManager* getInstance();
    GLint accquireBindingPoint();
    void releaseBindingPoint(const GLint& bindingPoint);
    ~UboBindingManager() = default;
};


	
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
    UboBindingManager* bindingManager;

    RLLogger* logger = nullptr;

	void prepareOffsetsAndDataType();

   public:
    UBO(GLuint shaderProgram, const std::string& uniformBlockName);

    // 绑定点可以看作shader ubo block上的插槽，需要先开辟插槽，插槽上才能链接一个ubo
    // 多个不同的shader可能享用相同的UBO，这是绑定点设计的核心逻辑
    // 所以会有以下关系出现
    // 0号绑定点对应摄像机位置的ubo，然后0号绑定点绑定了点光源的摄像机位置，又绑定了方向光的摄像机位置

    void bindAnotherShader(GLuint anohterProgram);

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
