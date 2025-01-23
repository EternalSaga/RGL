#include "Mesh.hpp"

namespace RGL {
namespace glcore {

FloatDescs Mesh::getDesc() const {
    return descs;
}
void Mesh::pushVertex(const std::vector<GLfloat>& vertex) {
    assert(vertex.size() == vertLength);
    channeledVertices.insert(channeledVertices.end(), vertex.begin(), vertex.end());
}
void Mesh::pushIndex(const int index) {
    indices.push_back(index);
}
Mesh::Mesh() : indices(), indicesCount(0),material() {
}
Mesh::Mesh(FloatDescs descs,size_t numOfVertcies) : descs(descs), indicesCount(0), indices(), channeledVertices(),material() {
	vertLength = getVertexLength();
    channeledVertices.resize(vertLength * numOfVertcies);
}
std::tuple<size_t, size_t> Mesh::getIdicesCountAndOffset() {
    if (indicesCount == 0 || indicesOffset == 0) {
	throw std::logic_error("Mesh::getIdicesCountAndOffset() called before setting indices");
    }
    return std::make_tuple(indicesCount, indicesOffset);
}
std::vector<int> Mesh::getIndices() const {
    return indices;
}
std::vector<GLfloat> Mesh::getChanneledVertices() const {
    return channeledVertices;
}
size_t Mesh::getIndicesCount() const {
    if (indicesCount == 0) {
	throw std::logic_error("Mesh::getIndicesCount() called before setting indices");
    }
    return indicesCount;
}
std::unique_ptr<VAO> VAOCreater::createMeshVAO(std::vector<Mesh> meshes,const Shader& shader) {
    auto vao = std::make_unique<VAO>(meshes.size());
    auto vbo = std::make_unique<VBO>(meshes.size());
    for (int i = 0; i < meshes.size(); i++) {

	vbo->setData(i, {meshes[i].getChanneledVertices(), meshes[i].getIndices()});
	vao->setShaderProgram(shader);
	vao->setDSA_interleaved(i, *vbo, meshes[i].getDesc());
	vao->addEBO((*vbo)[i]);
    }
    return std::move(vao);
}

std::unique_ptr<VAO> VAOCreater::createMeshVAO(const Mesh& mesh, const Shader& shader) {
    auto vao = std::make_unique<VAO>();
    auto vbo = std::make_unique<VBO>();

    vbo->setData({mesh.getChanneledVertices(), mesh.getIndices()});
    vao->setShaderProgram(shader);
    vao->setDSA_interleaved(0, *vbo, mesh.getDesc());
    vao->addEBO(*vbo);
    return std::move(vao);
}

size_t Mesh::getVertexLength() {
    size_t vertLength = 0;
    for (size_t i = 0; i < descs.size(); i++) {
	vertLength += descs[i].getLength();
    }
    return vertLength;
}
}  // namespace glcore
}
