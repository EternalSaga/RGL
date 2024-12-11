#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include "Shader.hpp"
#include "VertexDescriptor.hpp"

#include "GLObj.hpp"
namespace RGL {
namespace glcore {

class Mesh {
   protected:
    std::vector<int> indices;
    size_t indicesCount;
    std::vector<GLfloat> channeledVertices;
    FloatDescs descs;
    size_t vertLength = 0;
    size_t indicesOffset = 0;

    size_t getVertexLength();

   public:
    explicit Mesh();

    std::tuple<size_t, size_t> getIdicesCountAndOffset();

    std::vector<int> getIndices() const;
    std::vector<GLfloat> getChanneledVertices() const;
    size_t getIndicesCount() const;
    virtual ~Mesh() = default;
    FloatDescs getDesc() const;
};

namespace VAOCreater {

// 需要使用shader确定vao上的input attributes位置
std::unique_ptr<VAO> createMeshVAO(const Mesh& mesh, const Shader& shader);
std::unique_ptr<VAO> createMeshVAO(std::vector<Mesh> meshes, const Shader& shader);
}  // namespace VAOCreater

}  // namespace glcore
}  // namespace RGL
