#pragma once
#include <cstddef>
#include <type_traits>
#include <glad/glad.h>
#include "Helpers.hpp"
#include "rllogger.hpp"

#include <vector>
#include <string>
template <typename T>
concept IsStandardLayoutType = std::is_standard_layout_v<T>;
namespace RGL {
namespace glcore {
class SSBO {
    GLuint ssbo;
    GLint bindingPoint;
    size_t currentSize; 

public:
 SSBO(GLint bindingPoint);

 ~SSBO();

 void updateBuffer(const void* data, size_t size);

 void updateBufferSubData(const void* data, size_t size, size_t offset);

 template <typename T>
 void readBuffer(T& out_data, size_t size_to_read, size_t offset = 0) {
     assert(offset + size_to_read <= currentSize);
     glCall(glMemoryBarrier, GL_SHADER_STORAGE_BARRIER_BIT);
     glCall(glGetNamedBufferSubData, this->ssbo, offset, size_to_read, &out_data);
 }

    template<typename ElementType>
    void readBuffer(std::vector<ElementType>& out_vector, size_t offset = 0) {
        size_t size_to_read = out_vector.size() * sizeof(ElementType);
        assert(offset + size_to_read <= currentSize);
        glCall(glMemoryBarrier, GL_SHADER_STORAGE_BARRIER_BIT);
        glCall(glGetNamedBufferSubData, this->ssbo, offset, size_to_read, out_vector.data());
    }
};
}}