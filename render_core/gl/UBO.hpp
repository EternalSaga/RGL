#pragma once
#include <glad/glad.h>

#include "GLObj.hpp"



#include <array>
#include <unordered_map>
namespace RGL {
namespace glcore {

	
class UBO {
    GLuint ubo;

    GLint bindingPoint;

    size_t blockSize;




   public:
    UBO(GLint bindingPoint, size_t blockSize);


    void setUniform(const void* data);

    ~UBO();
};
using UBOs = std::shared_ptr<std::unordered_map<std::string, std::shared_ptr<UBO>>>;
}  // namespace glcore
}  // namespace RGL
