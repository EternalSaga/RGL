#include "UBO.hpp"
#include <cstddef>

namespace RGL {
namespace glcore {

std::unique_ptr<UboBindingManager> UboBindingManager::instance{nullptr};
std::once_flag UboBindingManager::initOnce;

void UBO::prepareOffsetsAndDataType() {
    blockIndex = glCall(glGetUniformBlockIndex, shader, uboName.c_str());
    if (blockIndex == GL_INVALID_INDEX) {
	throw std::invalid_argument("invalid block name or in activated block");
    }
    // 获取 Uniform Block 中 Uniform 的数量：
    glCall(glGetActiveUniformBlockiv, this->shader, blockIndex, GL_UNIFORM_BLOCK_DATA_SIZE, &blockSize);

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
}
UBO::UBO(GLuint shaderProgram, const std::string& uniformBlockName) {
    bindingManager = UboBindingManager::getInstance();
    this->uboName = uniformBlockName;
    shader = shaderProgram;

    prepareOffsetsAndDataType();

	cpuUbo = std::make_unique<UBOCPU>(blockSize, uniformOffsets, uniformTypes, uniformArrayStrides, uniformSizes, uniformNames);

    glCall(glCreateBuffers, 1, &ubo);
    glCall(glNamedBufferData, ubo, blockSize, nullptr, GL_DYNAMIC_DRAW);  // UBO是需要频繁修改的绘制数据，所以是GL_DYNAMIC_DRAW
    bindingPoint = bindingManager->accquireBindingPoint();
    glCall(glBindBufferBase, GL_UNIFORM_BUFFER, ubo, this->bindingPoint);  // 直接把ubo绑定到绑定点上

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
    cpuUbo->set(uboBlockName, value);
}
void UBO::setUniform() {
    glCall(glNamedBufferSubData, ubo, 0, blockSize, cpuUbo->getData());
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
	    break;
	}
    }
    if (bindingPoint == -1) {
	throw std::logic_error("no availiable binding point");
    }
    return bindingPoint;
}

void UboBindingManager::releaseBindingPoint(const GLint& bindingPoint) {
    assert(usedBindingPoints[bindingPoint] == true);
    usedBindingPoints[bindingPoint] = false;
}

UBOCPU::UBOCPU(size_t totalSize, const std::vector<GLint>& uniformOffsets, const std::vector<GLint>& uniformTypes, const std::vector<GLint>& uniformArrayStrides, const std::vector<GLint>& uniformSizes, const std::vector<std::string>& uniformNames) : sz(totalSize), uniformOffsets(uniformOffsets), uniformTypes(uniformTypes), uniformArrayStrides(uniformArrayStrides), uniformSizes(uniformSizes), uniformNames(uniformNames), data(totalSize) {
}

void UBOCPU::set(const std::string& uniformName, const CommonUniformTypes& value) {
    const auto it = std::find(uniformNames.begin(), uniformNames.end(), uniformName);
    if (it == uniformNames.end()) {
	throw std::invalid_argument("Uniform name not found.");
    }
    const auto index = std::distance(uniformNames.begin(), it);
    const auto offset = uniformOffsets[index];
    const auto type = uniformTypes[index];
    boost::variant2::visit([this, offset, type](const auto& trueValue) {
	if constexpr (std::is_same_v<decltype(trueValue), glm::vec3>) {
	    assert(type == GL_FLOAT_VEC3);
	}
	if constexpr (std::is_same_v<decltype(trueValue), glm::vec4>) {
	    assert(type == GL_FLOAT_VEC4);
	}
	if constexpr (std::is_same_v<decltype(trueValue), glm::mat3>) {
	    assert(type == GL_FLOAT_MAT3);
	}
	if constexpr (std::is_same_v<decltype(trueValue), glm::mat4>) {
	    assert(type == GL_FLOAT_MAT4);
	}
	if constexpr (std::is_same_v<decltype(trueValue), float>) {
	    assert(type == GL_FLOAT);
	}
	if constexpr (std::is_same_v<decltype(trueValue), GLint>) {
	    assert(type == GL_INT || type == GL_SAMPLER_2D);
	}
	if constexpr (std::is_same_v<decltype(trueValue), GLuint>) {
	    assert(type == GL_UNSIGNED_INT);
	} else {
	    throw std::invalid_argument("unsupported type");
	}
	std::memcpy(data.data() + offset, &trueValue, sizeof(trueValue));
    },
	value);
}

const std::byte* UBOCPU::getData() const {
    return data.data();
}

}  // namespace glcore

}  // namespace RGL
