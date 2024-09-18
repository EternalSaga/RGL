#include "Entity.hpp"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/random_generator.hpp>
#include <cassert>
#include "Mesh.hpp"
namespace RGL {
namespace glcore {

Entity::Entity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale, std::shared_ptr<Shader> shader, std::string name) : position(position), angleX(angleX), angleY(angleY), angleZ(angleZ), scale(scale), shader(shader), name(name) ,
vaoCreator(shader)
{

    uuid = boost::uuids::random_generator()();

}
void Entity::rotateX(float angle) {
    angleX += angle;
}
void Entity::rotateY(float angle) {
    angleY += angle;
}
void Entity::rotateZ(float angle) {
    angleZ += angle;
}
void Entity::setScale(const glm::vec3& scale) {
    this->scale = scale;
}
glm::mat4 Entity::getModelMatrix() {
    auto tranform{glm::identity<glm::mat4>()};

    assert(scale.x != 0.0f && scale.y != 0.0f && scale.z != 0.0f);

    tranform = glm::scale(tranform, scale);
    tranform = glm::rotate(tranform, glm::radians(angleX), glm::vec3(1.0f, 0.0f, 0.0f));
    tranform = glm::rotate(tranform, glm::radians(angleY), glm::vec3(0.0f, 1.0f, 0.0f));
    tranform = glm::rotate(tranform, glm::radians(angleZ), glm::vec3(0.0f, 0.0f, 1.0f));
    tranform = glm::translate(glm::identity<glm::mat4>(), position) * tranform;

    return tranform;
}
void Entity::draw() {
    shader->setUniformMat("modelMatrix", getModelMatrix());
    glCall(glBindVertexArray, *vao);
    glCall(glDrawElements, GL_TRIANGLES, mvertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(midxOffset));
}
void Entity::setMesh(std::unique_ptr<Mesh> mesh) {
    this->mesh = std::move(mesh);
    this->vao = vaoCreator.createMeshVAO(*this->mesh);
    auto [vertCount, idxOffset] = this->mesh->getIdicesCountAndOffset();
    mvertCount = vertCount;
    midxOffset = idxOffset;

}
void Entity::setMaterial(std::unique_ptr<Material> material) {
    this->material = std::move(material);
}
Mesh* Entity::getMesh() {
    return mesh.get();
}
boost::uuids::uuid Entity::getUUID() {
    return uuid;
}
void Entity::setPosition(const glm::vec3& position) {
    this->position = position;
}
SceneManager::SceneManager(std::shared_ptr<Shader> shader) : vaoCreator(shader), shader(shader) {
}
void SceneManager::addEntity(std::unique_ptr<Entity> entity) {
    const auto uuid = entity->getUUID();
    entities[uuid] = std::move(entity);

}
void SceneManager::removeEntity(const boost::uuids::uuid& uuid) {
    entities.erase(uuid);
}
void SceneManager::addLight(std::unique_ptr<Light> light) {
    const auto uuid = light->getUUID();
    this->lights[uuid] = std::move(light);
}
void SceneManager::drawALL() {
    for (auto& light : lights) {
	light.second->setShaderUniforms(shader.get());
    }
    for (auto& entity : entities) {
	entity.second->draw();
    }
}
}  // namespace glcore
}  // namespace RGL