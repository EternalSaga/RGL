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

	size_t getVertexLength() {
	size_t vertLength = 0;
	    for (size_t i = 0; i < descs.size(); i++) {
	    vertLength += descs[i].getLength();
	    }
	    return vertLength;
	}

   public:
    Mesh();

    std::tuple<size_t, size_t> getIdicesCountAndOffset();

	std::vector<int> getIndices() const;
	std::vector<GLfloat> getChanneledVertices() const;
    size_t getIndicesCount() const;
    virtual ~Mesh() = default;
    FloatDescs getDesc() const;
};

class VAOCreater {
    std::shared_ptr<Shader> shader;
    ;

   public:
    std::unique_ptr<VAO> createMeshVAO(const Mesh& mesh);
    std::unique_ptr<VAO> createMeshVAO(std::vector<Mesh> meshes);

    VAOCreater(std::shared_ptr<Shader> shader);
    ~VAOCreater() = default;
};

}  // namespace glcore
}  // namespace RGL
