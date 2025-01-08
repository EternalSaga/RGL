#include "Entity.hpp"

#include <cassert>
#include "Mesh.hpp"
#include "CameraECS.hpp"
#include "DataPipeline.hpp"
#include "UBO.hpp"
#include "SpotLight.hpp"
#include "PointLight.hpp"
#include "EnTTRelationship.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
namespace RGL {
namespace glcore {



CommonRenderEntity::CommonRenderEntity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale) : entity(singleReg->create()) {
    singleReg->emplace<Transform>(entity, position, glm::vec3(angleX,angleY,angleZ), scale);

    singleReg->emplace<DiscreteUniforms>(entity);
}

CommonRenderEntity::~CommonRenderEntity() {
    singleReg->destroy(entity);
}

void CommonRenderEntity::setMesh(std::unique_ptr<Mesh> mesh, ShaderRef shader) {
    auto vao = VAOCreater::createMeshVAO(*mesh, *shader);
    auto [vertCount, idxOffset] = mesh->getIdicesCountAndOffset();
    singleReg->emplace<VertArrayComponent>(entity, std::move(vao), vertCount, idxOffset);
    singleReg->emplace<ShaderRef>(entity, shader);
}
void CommonRenderEntity::setMaterial(std::unique_ptr<Material> material) {
    singleReg->emplace<MaterialComponent>(entity, std::move(material));
}

void CommonRenderEntity::update() {
    
    updateTransforms();
    modelSystemUBO();
    modelSystemSimple();
    materialSystem();
    updateSpotLight();
    updatePointLight();
    updateDirLight();
    renderVertexArray();
}
using namespace entt::literals;
void CommonRenderEntity::modelSystemUBO() {
    auto singleReg = EnttReg::getPrimaryRegistry();
    auto viewForModel = singleReg->view<const Transform, UBOs>();

    viewForModel.each([&singleReg](const entt::entity entity,const Transform& transform, UBOs& ubos) {



	const CameraProjection proj = singleReg->ctx().get<CameraProjection>("CameraProjection"_hs);

	const glm::vec3 camPosition = singleReg->ctx().get<glm::vec3>("cameraPos"_hs);

	const glm::mat4 MVP = proj.projMat * proj.viewMat * transform.modelMatrix;
	const glm::mat3 inverseModelMatrix = glm::transpose(glm::inverse(glm::mat3(transform.modelMatrix)));

	auto trans = (*ubos)["Transforms"];
	trans->updateCpuUbo("modelMatrix", transform.modelMatrix);
	trans->updateCpuUbo("MVP", MVP);
	trans->updateCpuUbo("inverseModelMatrix", glm::mat4(inverseModelMatrix));
	
    });
}

void CommonRenderEntity::modelSystemSimple() {
    auto singleReg = EnttReg::getPrimaryRegistry();
   
    // 对于简单模型来说，不需要UBO，所以直接排除UBO
    auto viewForModel = singleReg->view<const Transform, DiscreteUniforms>(entt::exclude<UBOs>);

    viewForModel.each([&singleReg](const entt::entity entity, const Transform& transform, DiscreteUniforms& uniforms) {

	
	const CameraProjection proj = singleReg->ctx().get<CameraProjection>("CameraProjection"_hs);
	const glm::vec3 camPosition = singleReg->ctx().get<glm::vec3>("cameraPos"_hs);
	const glm::mat4 MVP = proj.projMat * proj.viewMat * transform.modelMatrix;
	uniforms["MVP"] = MVP;
    });
}

void CommonRenderEntity::materialSystem() {
    auto singleReg = EnttReg::getPrimaryRegistry();

    auto viewForMaterial = singleReg->view<const MaterialComponent, DiscreteUniforms>();

    viewForMaterial.each([](const MaterialComponent& material, DiscreteUniforms& uniforms) {
	material.material->setShaderUniforms(uniforms);
    });
}

void CommonRenderEntity::renderVertexArray() {
    auto singleReg = EnttReg::getPrimaryRegistry();
    auto viewForVertexArray = singleReg->view<const VertArrayComponent, ShaderRef, DiscreteUniforms, UBOs>();

    viewForVertexArray.each([](const VertArrayComponent& mesh, ShaderRef shader, DiscreteUniforms& distUniform, UBOs& ubos) {
	ScopeShader scopeshader(*shader);
	updateAllUniforms(shader, distUniform);
	for (auto [blockName, ubo] : *ubos) {
	    ubo->setUniform();
	}

	glCall(glBindVertexArray, *mesh.vao);
	glCall(glDrawElements, GL_TRIANGLES, mesh.vertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(mesh.idxOffset));
	glCall(glBindVertexArray, 0);
    });

	auto simpleRenderView = singleReg->view<const VertArrayComponent, ShaderRef, DiscreteUniforms>(entt::exclude<UBOs>);
	simpleRenderView.each([](const VertArrayComponent& mesh, ShaderRef shader, DiscreteUniforms& distUniform) {

		ScopeShader scopeshader(*shader);
	    updateAllUniforms(shader, distUniform);
	    glCall(glBindVertexArray, *mesh.vao);
	    glCall(glDrawElements, GL_TRIANGLES, mesh.vertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(mesh.idxOffset));
	    glCall(glBindVertexArray, 0);
	});

}

GeneralEntity::~GeneralEntity() {
    singleReg->destroy(entity);
}

}  // namespace glcore
}  // namespace RGL
