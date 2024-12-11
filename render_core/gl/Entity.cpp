#include "Entity.hpp"
#include "Light.hpp"
#include <cassert>
#include "Mesh.hpp"
#include "CameraECS.hpp"
#include "DataPipeline.hpp"
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
CommonEntity::CommonEntity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale) : entity(singleReg->create()) {
    singleReg->emplace<PoseComponent>(entity, angleX, angleY, angleZ);
    singleReg->emplace<ScaleComponent>(entity, scale);
    singleReg->emplace<PositionComponent>(entity, position);
    singleReg->emplace<UniformComponent>(entity);
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

void CommonEntity::setMesh(std::unique_ptr<Mesh> mesh, ShaderRef shader) {
    auto vao = VAOCreater::createMeshVAO(*mesh, *shader);
    auto [vertCount, idxOffset] = mesh->getIdicesCountAndOffset();
    singleReg->emplace<VertArrayComponent>(entity, std::move(vao), vertCount, idxOffset);
    singleReg->emplace<ShaderRef>(entity, shader);
}
void CommonEntity::setMaterial(std::unique_ptr<Material> material) {
    singleReg->emplace<MaterialComponent>(entity, std::move(material));
}

void CommonEntity::setLight(std::unique_ptr<Light> light) {
    singleReg->emplace<std::unique_ptr<Light>>(entity, std::move(light));
}

void CommonEntity::update() {

    modelSystem();
    lightSystem();
    materialSystem();
    renderVertexArray();
}

void CommonEntity::modelSystem() {
    auto singleReg = EnttReg::getPrimaryRegistry();
    auto viewForModel = singleReg->view<PoseComponent, ScaleComponent, PositionComponent, UniformComponent>();

    viewForModel.each([](PoseComponent& pose, ScaleComponent& scale, PositionComponent& position, UniformComponent& uniforms) {
	const auto modelMatrix = GetModelMatrix(pose, position, scale);

	uniforms["modelMatrix"] = modelMatrix;
	SharingData* sharingData = SharingData::getInstance();

	CameraProjection proj = entt::any_cast<CameraProjection>(sharingData->getData("CameraProjection"));

	const glm::vec3 camPosition = entt::any_cast<glm::vec3>(sharingData->getData("cameraPos"));
	uniforms["cameraPos"] = camPosition;
	const glm::mat4 MVP = proj.projMat * proj.viewMat * modelMatrix;

	uniforms["MVP"] = MVP;

	const glm::mat3 inverseModelMatrix = glm::transpose(glm::inverse(modelMatrix));

	uniforms["inverseModelMatrix"] = inverseModelMatrix;
    });
}

void CommonEntity::materialSystem() {
    auto singleReg = EnttReg::getPrimaryRegistry();

    auto viewForMaterial = singleReg->view<const MaterialComponent, UniformComponent>();

    viewForMaterial.each([](const MaterialComponent& material, UniformComponent& uniforms) {
	material.material->setShaderUniforms(uniforms);
    });
}

void CommonEntity::lightSystem() {
    auto singleReg = EnttReg::getPrimaryRegistry();
    auto viewForLight = singleReg->view<const std::unique_ptr<Light>, UniformComponent>();
    viewForLight.each([](const std::unique_ptr<Light>& light, UniformComponent& uniforms) {
	light->setShaderUniforms(uniforms);
    });
}

void CommonEntity::renderVertexArray() {
    auto singleReg = EnttReg::getPrimaryRegistry();
    auto viewForVertexArray = singleReg->view<const VertArrayComponent, ShaderRef, UniformComponent>();

    viewForVertexArray.each([](const VertArrayComponent& mesh, ShaderRef shader, UniformComponent& uniforms) {
	ScopeShader scopeshader(*shader);

	updateAllUniforms(shader, uniforms);
	glCall(glBindVertexArray, *mesh.vao);
	glCall(glDrawElements, GL_TRIANGLES, mesh.vertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(mesh.idxOffset));
	glCall(glBindVertexArray, 0);
    });
}

entt::entity CommonEntity::getEntity() const {
    return entity;
}

}  // namespace glcore
}  // namespace RGL
