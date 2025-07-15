#include "SSBO.hpp"

#include <cassert>
#include <cstddef>
#include "rllogger.hpp"




namespace RGL {
namespace glcore {

SSBO::SSBO(GLint bindingPoint)
    : ssbo(0), bindingPoint(bindingPoint), currentSize(0) {
    glCall(glCreateBuffers, 1, &ssbo);
    glCall(glBindBufferBase, GL_SHADER_STORAGE_BUFFER, this->bindingPoint, ssbo);
}
SSBO::~SSBO() {
    glCall(glDeleteBuffers, 1, &ssbo);
}
void SSBO::updateBuffer(const void* data, size_t size) {
    if (size > currentSize) {
	// 重新分配缓冲区 (可以加一些策略，比如分配1.5倍大小，避免频繁重分配)
	glCall(glNamedBufferData, ssbo, size, data, GL_DYNAMIC_DRAW);
	currentSize = size;
    } else if (size > 0) {
	// 只更新数据，不重新分配
	glCall(glNamedBufferSubData, ssbo, 0, size, data);
    }
}
void SSBO::updateBufferSubData(const void* data, size_t size, size_t offset) {
    if (offset + size > currentSize) {
        RLLogger::getInstance()->critical("SSBO update out of bounds");
        throw GLLogicError("SSBO update out of bounds");
    }
    glCall(glNamedBufferSubData, ssbo, offset, size, data);
}
}  // namespace glcore
}  // namespace RGL