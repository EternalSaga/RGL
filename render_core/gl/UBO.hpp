#pragma once
#include <glad/glad.h>
#include "Helpers.hpp"
#include "GLObj.hpp"
#include "Shader.hpp"
namespace RGL {
namespace glcore {
template <typename T>
class UBO {
    GLuint ubo;
    bool isBinded = false;

   public:
    UBO() {
	glCreateBuffers(1, &ubo);
	glNamedBufferData(ubo, sizeof(T), nullptr, GL_DYNAMIC_STORAGE_BIT);
    }

    void setUniform(const T &t);
    // 绑定点可以看作shader ubo block上的插槽，需要先开辟插槽，插槽上才能链接一个ubo
    // 多个不同的shader可能享用相同的UBO，这是绑定点设计的核心逻辑
    // 所以会有以下关系出现
    // 0号绑定点对应摄像机位置的ubo，然后0号绑定点绑定了点光源的摄像机位置，又绑定了方向光的摄像机位置
    // 为什么不直接用ubo的数字当作绑定点？说实话可以，但是可能不方便动态绑定，ubo可以解绑一个绑定点，换到另一个绑定点。
    // 暂时先把ubo编号本身当作绑定点，遇到不能处理的情况再考虑unbind-rebind情况
    void bindToShader(std::shared_ptr<Shader> shader, const std::string &uboBlockName)(std::shared_ptr<Shader> shader, const std::string &uboBlockName) {
	assert(!isBinded);
	GLuint blockIndex = glCall(glGetUniformBlockIndex, *shader, uboBlockName.c_str());
	glCall(glUniformBlockBinding, *shader, blockIndex, ubo);  // 在shader ubo block上分配一个ubo号绑定点
	glCall(glBindBufferBase, GL_UNIFORM_BUFFER, ubo, ubo);	  // 直接把ubo绑定到ubo号绑定点
	isBinded = true;
    }

    void setUniform(const T &t) {
	static_assert(std::is_standard_layout_v<T>);
	assert(isBinded);

	glCall(glNamedBufferSubData, ubo, 0, sizeof(T), &t);
    }

    ~UBO() {
	glCall(glUnmapNamedBuffer, ubo);
	glCall(glDeleteBuffers, 1, &ubo);
    }
};

}  // namespace glcore
}  // namespace RGL
