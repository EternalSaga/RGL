#include <memory>

#include "Entity.hpp"
#include "Geometry.hpp"
#include "Light.hpp"
#include "Material.hpp"

#include "LightEffect.hpp"

namespace RGL {
namespace practice {

PhongSPMaskExec::PhongSPMaskExec(std::shared_ptr<Camera> cam) {
    this->cam = cam;
    ShaderSrcs shaders = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\phong.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\blinn-phong-mask.frag"}}};
    this->shader = std::make_shared<Shader>(shaders);
    // 初始化纹理
    box_spMask = std::make_unique<Texture>(2);
    {
	// 木头底色
	io::LoadedImg boxImg("./assest/box.png");
	box_spMask->set(boxImg, "box", true);
	// 金属光泽mask
	io::LoadedImg maskImg("./assest/sp_mask.png");
	box_spMask->set(maskImg, "spMask", true);
    }
    shader->useProgram();
    // 从纹理创建材质
    std::unique_ptr<Material> material = std::make_unique<PhoneWithSPMask>(box_spMask.get(), shader.get(), "box", 32.0f);
    material->setShaderUniforms();
    std::unique_ptr<Mesh> geometry = std::make_unique<Cube>(12.0f);
    std::unique_ptr<CommonEntity> cubeEntity = std::make_unique<CommonEntity>(glm::vec3{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, glm::vec3{1.0f, 1.0f, 1.0f}, shader);
    // 实体设置网格和纹理
    cubeEntity->setMesh(std::move(geometry));
    cubeEntity->setMaterial(std::move(material));
    this->scene = std::make_unique<SceneManager>(shader);

    // 光源
    std::unique_ptr<Light> light = std::make_unique<DirectionalLight>(glm::vec3{1.0f, 1.0f, -1.0f}, glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.2f, 0.2f, 0.2f}, 0.5f, 32.0f);
    // 场景添加光源
    this->scene->addLight(std::move(light));
}

void PhongSPMaskExec::operator()() {
    shader->useProgram();

    auto camProps = cam->update();

    shader->setUniformMat("viewMatrix", camProps.viewMat);
    shader->setUniformMat("projectionMatrix", camProps.projMat);
    shader->setUniform("cameraPos", camProps.position);
    // 场景绘制
    this->scene->drawALL();
}
SpotLightExec::SpotLightExec(std::shared_ptr<Camera> cam) {
}

void SpotLightExec::operator()() {
}
}  // namespace practice
}  // namespace RGL
