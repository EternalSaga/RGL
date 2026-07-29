#include "Mesh.hpp"
#include <glad/glad.h>
#include "GLCheckError.hpp"
#include "GLObj.hpp"
#include "GLTextures.hpp"
#include "Helpers.hpp"

#include "rllogger.hpp"

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
Mesh::Mesh() : indices(), material() {
}
Mesh::Mesh(FloatDescs descs, size_t numOfVertcies) : descs(descs), indices(), channeledVertices(), material() {
    vertLength = getVertexLength();
    const auto reserveSize = vertLength * numOfVertcies;

    channeledVertices.reserve(reserveSize);

    this->indicesOffset = reserveSize * sizeof(decltype(channeledVertices[0]));
}
std::tuple<size_t, size_t> Mesh::getIdicesCountAndOffset() {
    if (this->indices.empty() || this->channeledVertices.empty()) {
	throw std::logic_error("Mesh::getIdicesCountAndOffset() called before setting indices");
    }
    this->indicesOffset = this->channeledVertices.size()*sizeof(GLfloat);
    return std::make_tuple(this->indices.size(), indicesOffset);
}
std::vector<int> Mesh::getIndices() const {
    return indices;
}
std::vector<GLfloat> Mesh::getChanneledVertices() const {
    return channeledVertices;
}
size_t Mesh::getIndicesCount() const {
    if (indices.empty()) {
	throw std::logic_error("Mesh::getIndicesCount() called before setting indices");
    }
    return indices.size();
}
std::unique_ptr<VAO> VAOCreater::createMeshVAO(std::vector<Mesh> meshes, const Shader& shader) {
    auto vao = std::make_unique<VAO>(meshes.size());
    auto vbo = std::make_unique<VBO>(meshes.size());
    for (int i = 0; i < meshes.size(); i++) {
	vbo->setData(i, VerticesWithIndices{meshes[i].getChanneledVertices(), meshes[i].getIndices()});
	vao->setShaderProgram(shader);
	vao->setDSA_interleaved(i, *vbo, meshes[i].getDesc());
	vao->addEBO((*vbo)[i]);
    }
    return std::move(vao);
}

std::unique_ptr<VAO> VAOCreater::createMeshVAO(const Mesh& mesh, const Shader& shader) {
    auto vao = std::make_unique<VAO>();
    auto vbo = std::make_unique<VBO>();
    vbo->setData(VerticesWithIndices{mesh.getChanneledVertices(), mesh.getIndices()});
    vao->setShaderProgram(shader);
    vao->setDSA_interleaved(0, *vbo, mesh.getDesc());
    vao->addEBO(*vbo);
    return std::move(vao);
}

std::unique_ptr<VAO> VAOCreater::createMeshVAO(const Mesh& mesh,const std::vector<glm::mat4>& instanceMatrices, const Shader& shader){
    auto vao = std::make_unique<VAO>();
    auto vbo = std::make_unique<VBO>();
    vbo->setData(VerticesWithInstancesAndIndices{mesh.getChanneledVertices(), mesh.getIndices(), instanceMatrices});
    vao->setShaderProgram(shader);
    vao->setDSA_interleaved(0, *vbo, mesh.getDesc());
    vao->addEBO(*vbo);
    vao->addInstanceBuffer(0,*vbo);
    return std::move(vao);
}


size_t Mesh::getVertexLength() {
    size_t vertLength = 0;
    for (size_t i = 0; i < descs.size(); i++) {
	vertLength += descs[i].getLength();
    }
    return vertLength;
}

void Mesh::setMaterial(std::shared_ptr<AssetMaterialData> material) {
    materialHasSet = true;
    this->material = material;
}
std::shared_ptr<AssetMaterialData> Mesh::getMaterial() const {
    if (!materialHasSet) {
	auto logger = RGL::RLLogger::getInstance();
	logger->error("material not set, please set material first.");
    }
    return material;
}




size_t Mesh::getVertexCount() const {
    return channeledVertices.size() / vertLength;
}

void Mesh::setAABB(const AABB& aabb) {
    modelAABB = aabb;
}
AABB Mesh::getAABB() const {
    return modelAABB;
}
}  // namespace glcore
}  // namespace RGL
