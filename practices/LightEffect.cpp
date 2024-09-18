#include <glm/fwd.hpp>
#include <memory>
#include <vector>
#include "Entity.hpp"
#include "Geometry.hpp"
#include "Light.hpp"
#include "Material.hpp"
#include "VertexDescriptor.hpp"
#include "ProgrammedTexture.hpp"
#include "Helpers.hpp"

#include "LightEffect.hpp"

namespace RGL {
namespace practice {

GlobalLight::GlobalLight(std::shared_ptr<Camera> cam):light({1.0f, 1.0f, -1.0f}, {1.0f, 0.9f, 0.9f}, {0.1f, 0.1f, 0.1f}, 0.5f,32.0f)
 {
    this->cam = cam;
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\phong.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\phong.frag"}}};
    this->shader = std::make_shared<Shader>(shaders);

    checkboarder = std::make_unique<Texture>();
    {
	texture::CheckerBoard cb(8, 8);
	checkboarder->set(cb.getTexture(), "checkboarder", true);
	checkboarder->setFilltering("checkboarder", GL_NEAREST);
    }
    shader->useProgram();
    std::unique_ptr<Mesh> geometry = std::make_unique<Cube>(12.0f);

	VAOCreater vaoCreator(shader);

    this->vao = vaoCreator.createMeshVAO(*geometry);
    auto [vertCount, idxOffset] = geometry->getIdicesCountAndOffset();
    mvertCount = vertCount;
    midxOffset = idxOffset;
}
void GlobalLight::operator()() {
    glCall(glBindVertexArray, *vao);
    shader->useProgram();
    shader->setUniform("sampler", checkboarder->useTexture("checkboarder"));
    shader->setUniformMat("modelMatrix", this->modelMatrix);
    shader->setUniformMat("viewMatrix", cam->getViewMatrix());
    shader->setUniformMat("projectionMatrix", cam->getProjectionMatrix());

    light.setShaderUniforms(shader.get());


    shader->setUniform("cameraPos", cam->position);

    glCall(glDrawElements, GL_TRIANGLES, mvertCount, GL_UNSIGNED_INT, reinterpret_cast<void *>(midxOffset));
}

TestEntity::TestEntity(std::shared_ptr<Camera> cam){
    this->cam = cam;
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\phong.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\phong.frag"}}}; 
    this->shader = std::make_shared<Shader>(shaders);

    checkboarder = std::make_unique<Texture>();
    {
	texture::CheckerBoard cb(8, 8);
	checkboarder->set(cb.getTexture(), "checkboarder", true);
	checkboarder->setFilltering("checkboarder", GL_NEAREST);
    }
    std::unique_ptr<Material> material = std::make_unique<PhongMaterial>(checkboarder.get(),"checkboarder", 32.0f);
    shader->useProgram();
    material->setShaderUniforms(shader.get());
    std::unique_ptr<Mesh> geometry = std::make_unique<Cube>(12.0f);
    std::unique_ptr<Entity> cubeEntity = std::make_unique<Entity>(glm::vec3{0.0f,0.0f,0.0f},0.0f,0.0f,0.0f,glm::vec3{1.0f,1.0f,1.0f},shader,"cube");
    cubeEntity->setMesh(std::move(geometry));
    cubeEntity->setMaterial(std::move(material));
    this->scene = std::make_unique<SceneManager>(shader);
    this->scene->addEntity(std::move(cubeEntity));
    std::unique_ptr<Light> light = std::make_unique<DirectionalLight>(glm::vec3{1.0f, 1.0f, -1.0f}, glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.1f, 0.1f, 0.1f}, 0.5f,32.0f);
    this->scene->addLight(std::move(light));
}
void TestEntity::operator()() {
    shader->useProgram();
    shader->setUniform("sampler", checkboarder->useTexture("checkboarder"));
    shader->setUniformMat("viewMatrix", cam->getViewMatrix());
    shader->setUniformMat("projectionMatrix", cam->getProjectionMatrix());
    shader->setUniform("cameraPos", cam->position);
    this->scene->drawALL();
}
}  // namespace practice
}  // namespace RGL
