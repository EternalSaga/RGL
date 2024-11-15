#include "Entity.hpp"

#include <cassert>
#include "Mesh.hpp"
namespace RGL {
namespace glcore {
// void CommonEntity::rotateX(float angle) {
//     angleX += angle;
// }
// void CommonEntity::rotateY(float angle) {
//     angleY += angle;
// }
// void CommonEntity::rotateZ(float angle) {
//     angleZ += angle;
// }
// void CommonEntity::setScale(const glm::vec3& scale) {
//     this->scale = scale;
// }
CommonEntity::CommonEntity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale, std::shared_ptr<Shader> shader) : entity(singleReg->create()), vaoCreator(shader), shader(shader) {
    singleReg->emplace<PoseComponent>(entity, angleX, angleY, angleZ, scale, position);
}

glm::mat4 GetModelMatrix(const PoseComponent& pose) {
    auto tranform{glm::identity<glm::mat4>()};

    assert(pose.scale.x != 0.0f && pose.scale.y != 0.0f && pose.scale.z != 0.0f);

    tranform = glm::scale(tranform, pose.scale);
    tranform = glm::rotate(tranform, glm::radians(pose.angleX), glm::vec3(1.0f, 0.0f, 0.0f));
    tranform = glm::rotate(tranform, glm::radians(pose.angleY), glm::vec3(0.0f, 1.0f, 0.0f));
    tranform = glm::rotate(tranform, glm::radians(pose.angleZ), glm::vec3(0.0f, 0.0f, 1.0f));
    tranform = glm::translate(glm::identity<glm::mat4>(), pose.position) * tranform;

    return tranform;
}

void CommonEntity::setMesh(std::unique_ptr<Mesh> mesh) {
    auto vao = this->vaoCreator.createMeshVAO(*mesh);
    auto [vertCount, idxOffset] = mesh->getIdicesCountAndOffset();
    singleReg->emplace<MeshComponent>(entity, std::move(vao), vertCount, idxOffset);
}
void CommonEntity::setMaterial(std::unique_ptr<Material> material) {
    singleReg->emplace<MaterialComponent>(entity, std::move(material), shader);
}

SceneManager::SceneManager(std::shared_ptr<Shader> shader) : vaoCreator(shader), shader(shader) {
}

void SceneManager::addLight(std::unique_ptr<Light> light) {
    const auto uuid = light->getUUID();
    this->lights[uuid] = std::move(light);
}
void SceneManager::drawALL() {
    for (auto& light : lights) {
	light.second->setShaderUniforms(shader.get());
    }

    auto viewForCommonEntity = singleReg->view<PoseComponent, const MeshComponent, const MaterialComponent>();

    viewForCommonEntity.each([](PoseComponent& pose, const MeshComponent& mesh, const MaterialComponent& material) {
	material.shader->setUniformMat("modelMatrix", GetModelMatrix(pose));

	material.material->setShaderUniforms();

	glCall(glBindVertexArray, *mesh.vao);
	glCall(glDrawElements, GL_TRIANGLES, mesh.vertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(mesh.idxOffset));
    });
}
}  // namespace glcore
}  // namespace RGL
