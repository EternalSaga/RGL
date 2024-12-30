#include "Entity.hpp"
#include "Light.hpp"
#include <cassert>
#include "Mesh.hpp"
#include "CameraECS.hpp"
#include "DataPipeline.hpp"
#include "UBO.hpp"
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

CommonEntity::CommonEntity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale) : entity(singleReg->create()) {
    singleReg->emplace<PoseComponent>(entity, angleX, angleY, angleZ);
    singleReg->emplace<ScaleComponent>(entity, scale);
    singleReg->emplace<PositionComponent>(entity, position);
    singleReg->emplace<DiscreteUniforms>(entity);
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

void CommonEntity::update() {

    modelSystem();

    materialSystem();
    renderVertexArray();
}
using namespace entt::literals;
void CommonEntity::modelSystem() {
    auto singleReg = EnttReg::getPrimaryRegistry();
    auto viewForModel = singleReg->view<PoseComponent, ScaleComponent, PositionComponent, UBOs>();

    viewForModel.each([&singleReg](PoseComponent& pose, ScaleComponent& scale, PositionComponent& position, UBOs& ubos) {
	const auto modelMatrix = GetModelMatrix(pose, position, scale);



	const CameraProjection proj = singleReg->ctx().get<CameraProjection>("CameraProjection"_hs);


	 const glm::vec3 camPosition = singleReg->ctx().get<glm::vec3>("cameraPos"_hs);


	const glm::mat4 MVP = proj.projMat * proj.viewMat * modelMatrix;
	const glm::mat3 inverseModelMatrix = glm::transpose(glm::inverse(modelMatrix));


	auto trans = (*ubos)["Transforms"];
	trans->updateCpuUbo("modelMatrix", modelMatrix);
	trans->updateCpuUbo("MVP", MVP);
	trans->updateCpuUbo("inverseModelMatrix", inverseModelMatrix);



    });
}

void CommonEntity::materialSystem() {
    auto singleReg = EnttReg::getPrimaryRegistry();

    auto viewForMaterial = singleReg->view<const MaterialComponent,DiscreteUniforms>();

    viewForMaterial.each([](const MaterialComponent& material, DiscreteUniforms& uniforms) {
	material.material->setShaderUniforms(uniforms);
    });
}



void CommonEntity::renderVertexArray() {
    auto singleReg = EnttReg::getPrimaryRegistry();
    auto viewForVertexArray = singleReg->view<const VertArrayComponent, ShaderRef, DiscreteUniforms>();

    viewForVertexArray.each([](const VertArrayComponent& mesh, ShaderRef shader, DiscreteUniforms& uniforms) {
	ScopeShader scopeshader(*shader);
	updateAllUniforms(shader, uniforms);
	glCall(glBindVertexArray, *mesh.vao);
	glCall(glDrawElements, GL_TRIANGLES, mesh.vertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(mesh.idxOffset));
	glCall(glBindVertexArray, 0);
    });
}



}  // namespace glcore
}  // namespace RGL
