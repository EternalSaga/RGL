#include <memory>

#include "Entity.hpp"
#include "Geometry.hpp"
#include "Light.hpp"
#include "PointLight.hpp"
#include "SpotLight.hpp"
#include "Material.hpp"
#include <SDL2/SDL_timer.h>
#include "LightEffect.hpp"
#include "ShaderManager.hpp"
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
    // 从纹理创建材质
    std::unique_ptr<Material> material = std::make_unique<PhoneWithSPMask>(box_spMask.get(), "box");

    std::unique_ptr<Mesh> geometry = std::make_unique<Cube>(12.0f);
    std::unique_ptr<CommonEntity> cubeEntity = std::make_unique<CommonEntity>(glm::vec3{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, glm::vec3{1.0f, 1.0f, 1.0f});
    // 实体设置网格和纹理
    cubeEntity->setMesh(std::move(geometry), shader);
    cubeEntity->setMaterial(std::move(material));
    // 光源
    std::unique_ptr<Light> light = std::make_unique<DirectionalLight>(glm::vec3{1.0f, 1.0f, -1.0f}, glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.2f, 0.2f, 0.2f}, 32.0f);
    cubeEntity->setLight(std::move(light));
}

void PhongSPMaskExec::operator()() {
    cam->update();
    //  场景绘制
    CommonEntity::update();
}

void updatePosUniform(entt::registry& reg, entt::entity lightEntity) {
    auto& lightPos = reg.get<PositionComponent>(lightEntity).position;

    auto view = reg.view<UniformComponent, RenderTag>();
    view.each([&lightPos](UniformComponent& uniforms) {
	uniforms["lightPosition"] = lightPos;
    });
}
PointLightExec::PointLightExec(std::shared_ptr<Camera> cam) {
    this->cam = cam;
    ShaderSrcs pointLightshaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\phong.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\pointlight.frag"}}};
    this->pointlightShader = std::make_shared<Shader>(pointLightshaderSrc);
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
    // 从纹理创建材质
    std::unique_ptr<Material> material = std::make_unique<PhoneWithSPMask>(box_spMask.get(), "box");
    std::unique_ptr<Mesh> geometry = std::make_unique<Cube>(12.0f);
    std::unique_ptr<CommonEntity> cubeEntity = std::make_unique<CommonEntity>(glm::vec3{-10.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, glm::vec3{1.0f, 1.0f, 1.0f});
    // 实体设置网格和纹理,同时绑定下shader
    cubeEntity->setMesh(std::move(geometry), pointlightShader);
    cubeEntity->setMaterial(std::move(material));
    // 光源
    std::unique_ptr<Light> light = std::make_unique<PointLight>(glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.1f, 0.2f, 0.2f}, 32.0f, 0.017f, 0.07f, 1.0f);
    cubeEntity->setLight(std::move(light));
    // cube因为有光照相关shader，所以加个render tag，方便entt view筛选
    cubeEntity->attachComponent<RenderTag>();

    // 点光源白球相关初始化
    ShaderSrcs whiteShaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\white.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\white.frag"}}};
    this->whiteShader = std::make_shared<Shader>(whiteShaderSrc);
    std::unique_ptr<Mesh> sphereMesh = std::make_unique<Sphere>(2.2f);
    sphereEntity = std::make_unique<CommonEntity>(glm::vec3{10.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, glm::vec3{1.0f, 1.0f, 1.0f});
    sphereEntity->attachComponent<IsLight>();
    // 设置网格和shader
    sphereEntity->setMesh(std::move(sphereMesh), whiteShader);

    // 创建光源位置更新相关回调
    // 当点光源position发生变化的时候，更新cube等entitiy的shader position uniform
    singleReg->on_update<PositionComponent>().connect<&updatePosUniform>();
}

void PointLightExec::operator()() {
    cam->update();
    // 让光源动动
    singleReg->patch<PositionComponent>(*sphereEntity, [](PositionComponent& pos) {
	float time = static_cast<float>(SDL_GetTicks());
	pos.position = glm::vec3{std::sin(time * 0.01), 0.0f, 0.0f};
    });
    // 场景绘制
    CommonEntity::update();
}

SpotLightExec::SpotLightExec(std::shared_ptr<Camera> cam) {
    this->cam = cam;
    ShaderSrcs pointLightshaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\phong.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\spotlight.frag"}}};
    this->pointlightShader = std::make_shared<Shader>(pointLightshaderSrc);
    // 初始化纹理
    box_texture = std::make_unique<Texture>(1);
    {
	// 木头底色
	io::LoadedImg boxImg("./assest/box.png");
	box_texture->set(boxImg, "box", true);
    }
    // 从纹理创建材质
    std::unique_ptr<Material> material = std::make_unique<PhongMaterial>(box_texture.get(), "box");
    std::unique_ptr<Mesh> geometry = std::make_unique<Cube>(12.0f);
    std::unique_ptr<CommonEntity> cubeEntity = std::make_unique<CommonEntity>(glm::vec3{-10.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, glm::vec3{1.0f, 1.0f, 1.0f});
    // 实体设置网格和纹理,同时绑定下shader
    cubeEntity->setMesh(std::move(geometry), pointlightShader);
    cubeEntity->setMaterial(std::move(material));
    // 光源
    std::unique_ptr<Light> light = std::make_unique<SpotLight>(glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.1f, 0.2f, 0.2f}, 32.0f, 30.0f, 45.0f, glm::vec3{-1.0f, 0.0f, 0.0f}, glm::vec3{1.5f, 0.0f, 0.0f});
    cubeEntity->setLight(std::move(light));
    // cube因为有光照相关shader，所以加个render tag，方便entt view筛选
    cubeEntity->attachComponent<RenderTag>();

    // 点光源白球相关初始化
    ShaderSrcs whiteShaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\white.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\white.frag"}}};
    this->whiteShader = std::make_shared<Shader>(whiteShaderSrc);
    std::unique_ptr<Mesh> sphereMesh = std::make_unique<Sphere>(2.2f);
    sphereEntity = std::make_unique<CommonEntity>(glm::vec3{10.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, glm::vec3{1.0f, 1.0f, 1.0f});
    sphereEntity->attachComponent<IsLight>();
    // 设置网格和shader
    sphereEntity->setMesh(std::move(sphereMesh), whiteShader);

    // 创建光源位置更新相关回调
    // 当点光源position发生变化的时候，更新cube等entitiy的shader position uniform
    singleReg->on_update<PositionComponent>().connect<&updatePosUniform>();
}
void SpotLightExec::operator()() {
    cam->update();
    // 让光源动动
    singleReg->patch<PositionComponent>(*sphereEntity, [](PositionComponent& pos) {
	float time = static_cast<float>(SDL_GetTicks());
	pos.position = glm::vec3{std::sin(time * 0.01), 0.0f, 0.0f};
    });
    // 场景绘制
    CommonEntity::update();
}
}  // namespace practice
}  // namespace RGL
