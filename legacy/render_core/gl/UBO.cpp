#include "UBO.hpp"

#include "Helpers.hpp"
namespace RGL {
namespace glcore {


UBO::UBO(GLint bindingPoint,size_t blockSize) {
    this->blockSize = blockSize;
    glCall(glCreateBuffers, 1, &ubo);
    glCall(glNamedBufferData, ubo, blockSize, nullptr, GL_DYNAMIC_DRAW);  // UBO是需要频繁修改的绘制数据，所以是GL_DYNAMIC_DRAW
    this->bindingPoint = bindingPoint;
    glCall(glBindBufferBase, GL_UNIFORM_BUFFER, this->bindingPoint, ubo);  // 直接把ubo绑定到绑定点上

}

void UBO::setUniform(const void* data) {
    glCall(glNamedBufferSubData, ubo, 0, blockSize, data);
}

UBO::~UBO() {
   
    glCall(glDeleteBuffers, 1, &ubo);
}


}  // namespace glcore

}  // namespace RGL
