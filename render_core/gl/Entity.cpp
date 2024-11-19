#include "Entity.hpp"
#include "Light.hpp"
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
    singleReg->emplace<PoseComponent>(entity, angleX, angleY, angleZ);
    singleReg->emplace<ScaleComponent>(entity, scale);
    singleReg->emplace<PositionComponent>(entity, position);
}

glm::mat4 GetModelMatrix(const PoseComponent& pose, const PositionComponent& position, const ScaleComponent& scale) {
    auto tranform{glm::identity<glm::mat4>()};

    assert(scale.scale.x != 0.0f && scale.scale.y != 0.0f && scale.scale.z != 0.0f);

    tranform = glm::scale(tranform, scale.scale);
    tranform = glm::rotate(tranform, glm::radians(pose.angleX), glm::vec3(1.0f, 0.0f, 0.0f));
    tranform = glm::rotate(tranform, glm::radians(pose.angleY), glm::vec3(0.0f, 1.0f, 0.0f));
    tranform = glm::rotate(tranform, glm::radians(pose.angleZ), glm::vec3(0.0f, 0.0f, 1.0f));
    tranform = glm::translate(glm::identity<glm::mat4>(), position.position) * tranform;

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

SceneManager::SceneManager() {
}


void SceneManager::drawALL() {


	auto directionalLightEntitis = singleReg->view<const CommonLightComponent, const Direction, const std::shared_ptr<Shader>>();

	directionalLightEntitis.each([](const CommonLightComponent& directLight,const Direction& direction, const std::shared_ptr<Shader>& shader) {
	    shader->setUniform("ambient", directLight.ambientColor);
	    shader->setUniform("specularIntensity", directLight.specularIntensity);
	    shader->setUniform("lightColor", directLight.lightColor);
	    shader->setUniform("globalLightDirection", direction.direction);
		});


    auto viewForCommonEntity = singleReg->view<PoseComponent, ScaleComponent,PositionComponent, const MeshComponent, const MaterialComponent>();

    viewForCommonEntity.each([](PoseComponent& pose, ScaleComponent& scale, PositionComponent& position, const MeshComponent& mesh, const MaterialComponent& material) {
	material.shader->setUniformMat("modelMatrix", GetModelMatrix(pose, position,scale));

	material.material->setShaderUniforms();

	glCall(glBindVertexArray, *mesh.vao);
	glCall(glDrawElements, GL_TRIANGLES, mesh.vertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(mesh.idxOffset));
    });
}
}  // namespace glcore
}  // namespace RGL
