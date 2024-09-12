#include <vector>
#include "Geometry.hpp"
#include "Light.hpp"
#include "VertexDescriptor.hpp"
#include "ProgrammedTexture.hpp"
#include "Helpers.hpp"

#include "LightEffect.hpp"

namespace RGL {
namespace practice {

GlobalLight::GlobalLight(std::shared_ptr<Camera> cam):light({1.0f, 1.0f, -1.0f}, {1.0f, 0.9f, 0.9f}, {0.1f, 0.1f, 0.1f}, 0.5f)
 {
    this->cam = cam;
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\phong.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\blinn-phong.frag"}}};
    this->shader = std::make_unique<Shader>(shaders);
    this->shader = std::make_unique<Shader>(shaders);
    checkboarder = std::make_unique<Texture>();
    {
	texture::CheckerBoard cb(8, 8);
	checkboarder->set(cb.getTexture(), "checkboarder", true);
	checkboarder->setFilltering("checkboarder", GL_NEAREST);
    }
    shader->useProgram();
    std::unique_ptr<CommonGeometry> geometry = std::make_unique<Cube>(12.0f, *shader);
    this->vao = std::move(geometry->getVAO());
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

}  // namespace practice
}  // namespace RGL
