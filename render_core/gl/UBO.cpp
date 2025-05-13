#include "UBO.hpp"
#include <cstddef>
#include "rllogger.hpp"
namespace RGL {
namespace glcore {

std::unique_ptr<UboBindingManager> UboBindingManager::instance{nullptr};
std::once_flag UboBindingManager::initOnce;

std::string type2string(int gltype) {
    std::string typestr;
    switch (gltype) {
    case GL_FLOAT_VEC3:
	typestr = "VEC3";
	break;
    case GL_FLOAT_VEC4:
	typestr = "VEC4";
	break;
    case GL_FLOAT_MAT3:
	typestr = "MAT3";
	break;
    case GL_FLOAT_MAT4:
	typestr = "MAT4";
	break;
    case GL_FLOAT:
	typestr = "FLOAT";
	break;
    case GL_INT:
	typestr = "INT";
	break;
    case GL_UNSIGNED_INT:
	typestr = "UINT";
	break;
    default:
	throw std::logic_error("Unsupported GL type.");
    }
	return typestr;
}


void UBO::prepareOffsetsAndDataType() {
    blockIndex = glCall(glGetUniformBlockIndex, shader, uboName.c_str());
    if (blockIndex == GL_INVALID_INDEX) {
    logger->critical("Ubo blockname not found {} in shader program {}", uboName,shader);
	throw std::invalid_argument("invalid block name or in activated block");
    }
    // 获取 Uniform Block 中 Uniform 的数量：
    glCall(glGetActiveUniformBlockiv, this->shader, blockIndex, GL_UNIFORM_BLOCK_DATA_SIZE, &blockSize);
    data.resize(blockSize);
	//初始化数组为0
    std::fill(data.begin(), data.end(), std::byte(0));
    glCall(glGetActiveUniformBlockiv, this->shader, blockIndex, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &uniformCount);
    // 获取 Uniform 的索引：
    std::vector<GLuint> uniformIndices(uniformCount);
    glCall(glGetActiveUniformBlockiv, this->shader, blockIndex, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, (GLint*)uniformIndices.data());
    // 获取 Uniform 的信息（名称、类型、偏移量、数组步幅）
    uniformOffsets.resize(uniformCount);
    uniformTypes.resize(uniformCount);
    uniformArrayStrides.resize(uniformCount);
    uniformSizes.resize(uniformCount);	// uniform 元素个数，用于数组

    glCall(glGetActiveUniformsiv, shader, uniformCount, uniformIndices.data(), GL_UNIFORM_OFFSET, uniformOffsets.data());
    glCall(glGetActiveUniformsiv, shader, uniformCount, uniformIndices.data(), GL_UNIFORM_TYPE, uniformTypes.data());
    glCall(glGetActiveUniformsiv, shader, uniformCount, uniformIndices.data(), GL_UNIFORM_ARRAY_STRIDE, uniformArrayStrides.data());
    glCall(glGetActiveUniformsiv, shader, uniformCount, uniformIndices.data(), GL_UNIFORM_SIZE, uniformSizes.data());

    uniformNames.resize(uniformCount);
    for (int i = 0; i < uniformCount; ++i) {
	GLint nameLength;
	glCall(glGetActiveUniformsiv, shader, 1, &uniformIndices[i], GL_UNIFORM_NAME_LENGTH, &nameLength);
	uniformNames[i].resize(nameLength - 1);	 // 减去 null 终止符
	glCall(glGetActiveUniformName, shader, uniformIndices[i], nameLength, nullptr, uniformNames[i].data());
    }

	 
    for (size_t i = 0; i < uniformCount; i++) {
	    logger->info("uniformName:{};offset:{};type:{}\n", uniformNames[i], uniformOffsets[i], type2string(uniformTypes[i]));
    }
}
UBO::UBO(GLuint shaderProgram, const std::string& uniformBlockName) {
    logger = RLLogger::getInstance();
    bindingManager = UboBindingManager::getInstance();
    assert(bindingManager != nullptr);	// 增加断言检查
    this->uboName = uniformBlockName;
    shader = shaderProgram;

    prepareOffsetsAndDataType();


    glCall(glCreateBuffers, 1, &ubo);
    glCall(glNamedBufferData, ubo, blockSize, nullptr, GL_DYNAMIC_DRAW);  // UBO是需要频繁修改的绘制数据，所以是GL_DYNAMIC_DRAW
    bindingPoint = bindingManager->accquireBindingPoint();
    glCall(glBindBufferBase, GL_UNIFORM_BUFFER, this->bindingPoint, ubo);  // 直接把ubo绑定到绑定点上

    assert(this->bindingPoint != -1);
    glCall(glUniformBlockBinding, shader, blockIndex, this->bindingPoint);  // 在shader ubo block上分配一个绑定点
}

void UBO::bindAnotherShader(GLuint anohterProgram) {
    assert(this->bindingPoint != -1);
    assert(anohterProgram != shader);
    auto anotherBlockIndex = glCall(glGetUniformBlockIndex, anohterProgram, uboName.c_str());
    glCall(glUniformBlockBinding, anohterProgram, anotherBlockIndex, this->bindingPoint);  // 在shader ubo block上分配一个绑定点
}
void UBO::updateCpuUbo(const std::string& uboBlockName, const CommonUniformTypes& value) {
    const auto it = std::find(uniformNames.begin(), uniformNames.end(), uboBlockName);
    if (it == uniformNames.end()) {
	throw std::invalid_argument("Uniform name not found.");
    }
    const auto index = std::distance(uniformNames.begin(), it);
    const auto offset = uniformOffsets[index];
    const auto gltype = uniformTypes[index];
    boost::variant2::visit([this, offset,gltype](const auto& trueValue) {
	assert(offset + sizeof(trueValue) <= data.size());  // 防止越界
	using T = std::decay_t<decltype(trueValue)>;
	switch (gltype) {
	case GL_FLOAT_VEC3:
	    assert((std::is_same_v<T, glm::vec3>));
	    break;
	case GL_FLOAT_VEC4:
	    assert((std::is_same_v<T, glm::vec4>));
	    break;
	case GL_FLOAT_MAT3:
	    assert((std::is_same_v<T, glm::mat3>));
	    break;
	case GL_FLOAT_MAT4:
	    assert((std::is_same_v<T, glm::mat4>));
	    break;
	case GL_FLOAT:
	    assert((std::is_same_v<T, float>));
	    break;
	case GL_INT:
	    assert((std::is_same_v<T, int>));
	    break;
	case GL_UNSIGNED_INT:
	    assert((std::is_same_v<T, GLuint>));
	    break;
	default:
	    throw std::logic_error("Unsupported GL type.");
	}
	std::memcpy(data.data() + offset, &trueValue, sizeof(trueValue));
    },
	value);

}
void UBO::setUniform() {
    glCall(glNamedBufferSubData, ubo, 0, blockSize, data.data());
}

UBO::~UBO() {
    bindingManager->releaseBindingPoint(this->bindingPoint);
    // glCall(glUnmapNamedBuffer, ubo);
    glCall(glDeleteBuffers, 1, &ubo);
}

UboBindingManager::UboBindingManager() {
    for (size_t i = 0; i < MAX_BINDING_POINTS; i++) {
	usedBindingPoints[i] = false;
    }
}

UboBindingManager* UboBindingManager::getInstance() {
    std::call_once(initOnce, []() {
	instance.reset(new UboBindingManager());
    });
    return instance.get();
}

GLint UboBindingManager::accquireBindingPoint() {
    GLint bindingPoint = -1;
    for (size_t i = 0; i < MAX_BINDING_POINTS; i++) {
	if (!usedBindingPoints[i]) {
	    bindingPoint = i;
	    usedBindingPoints[i] = true;  // 更新状态
	    break;
	}
    }
    if (bindingPoint == -1) {
	throw std::logic_error("no availiable binding point");
    }
    return bindingPoint;
}

void UboBindingManager::releaseBindingPoint(const GLint& bindingPoint) {
    assert(bindingPoint >= 0 && bindingPoint < MAX_BINDING_POINTS);  // 增加范围检查
	if (usedBindingPoints[bindingPoint] == false) {
	throw std::logic_error("binding point not used.");
	}
    usedBindingPoints[bindingPoint] = false;
}



}  // namespace glcore

}  // namespace RGL
