#pragma once
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
    GLint blockSize;
    GLuint blockIndex;
    GLint storagesCount;

    std::vector<GLint> storageOffsets;
    std::vector<GLint> storageTypes;

    std::vector<GLint> storageArrayStrides;

    std::vector<GLint> storageSizes;

    std::vector<std::string> storageNames;

    std::vector<std::byte> data;

    GLuint shader;
    std::string ssboName;

    GLint bindingPoint;

    RLLogger* logger = nullptr;
    void prepareOffsetsAndDataType();

   public:
    SSBO(GLuint shader, std::string ssboName,GLint bindingPoint);


    void updateBufferData(std::vector<std::byte> cpuData);

    template<IsStandardLayoutType StandardLayoutType>
    StandardLayoutType getBlockData() {
        assert(sizeof(StandardLayoutType) == blockSize);

        glCall(glMemoryBarrier, GL_SHADER_STORAGE_BARRIER_BIT);
        StandardLayoutType cpuData;

        glCall(glGetNamedBufferSubData,this->ssbo,0,sizeof(StandardLayoutType), &cpuData);
        return cpuData;
    }
    
};
}}