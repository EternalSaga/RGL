#include "Entity.hpp"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/random_generator.hpp>
#include "Mesh.hpp"
namespace RGL {
namespace glcore {

Entity::Entity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale, std::shared_ptr<Shader> shader, std::string name) : position(position), angleX(angleX), angleY(angleY), angleZ(angleZ), scale(scale), shader(shader), name(name) ,
vaoCreator(shader)
{

    uuid = boost::uuids::random_generator()();
    this->vao = vaoCreator.createMeshVAO(*this->mesh);
    auto [vertCount, idxOffset] = mesh->getIdicesCountAndOffset();
    mvertCount = vertCount;
    midxOffset = idxOffset;
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
    tranform = glm::scale(tranform, scale);
    tranform = glm::rotate(tranform, glm::radians(angleX), glm::vec3(1.0f, 0.0f, 0.0f));
    tranform = glm::rotate(tranform, glm::radians(angleY), glm::vec3(0.0f, 1.0f, 0.0f));
    tranform = glm::rotate(tranform, glm::radians(angleZ), glm::vec3(0.0f, 0.0f, 1.0f));
    tranform = glm::translate(glm::identity<glm::mat4>(), position) * tranform;

   
    return tranform;
}
void Entity::draw() {
    glCall(glBindVertexArray, *vao);
    glCall(glDrawElements, GL_TRIANGLES, mvertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(midxOffset));
}
}  // namespace glcore
}  // namespace RGL