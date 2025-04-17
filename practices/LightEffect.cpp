#include <memory>

#include "Entity.hpp"
#include "Geometry.hpp"
#include "Light.hpp"
#include "PointLight.hpp"
#include "SpotLight.hpp"
#include "Material.hpp"

#include "LightEffect.hpp"
#include "ShaderManager.hpp"
#include "rllogger.hpp"
#include "UBO.hpp"
namespace RGL {
namespace practice {

UBOTest::UBOTest(std::shared_ptr<Camera> cam) {
    this->cam = cam;
    ShaderSrcs pointLightshaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\phong_ubo.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\spotlight_ubo.frag"}}};
    this->spotlightShader = std::make_shared<Shader>(pointLightshaderSrc);

    lightUBO = std::make_shared<UBO>(*spotlightShader, "SpotLight");
    transformUBO = std::make_shared<UBO>(*spotlightShader, "Transforms");
    ubos = std::make_shared<std::unordered_map<std::string, std::shared_ptr<UBO>>>();
    (*ubos)[lightUBO->getUboName()] = lightUBO;
    (*ubos)[transformUBO->getUboName()] = transformUBO;

    // 初始化纹理
    box_texture = textureCache.getTexture("assest/box.png", TextureUsageType::DIFFUSE);

    std::unique_ptr<Mesh> geometry = std::make_unique<Cube>(12.0f);

    material = std::make_shared<MaterialData>();
    // 从材质创建纹理
    material->appendTexture(box_texture);

    geometry->setMaterial(material);

    cubeEntity = std::make_unique<CommonRenderEntity>(glm::vec3{-10.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, glm::vec3{1.0f, 1.0f, 1.0f});

    // 设置网格和shader
    cubeEntity->setMesh(std::move(geometry), spotlightShader);

    // 给cube添加共享的ubos
    singleReg->emplace_or_replace<UBOs>(*cubeEntity, ubos);
    // cube因为有光照相关shader，所以加个render tag，方便entt view筛选
    cubeEntity->attachComponent<RenderTag>();

    spotLight = std::make_unique<GeneralEntity>();
    // 光源entity
    spotLight->attachComponent<CommonLight>(glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.1f, 0.2f, 0.2f}, 32.0f);
    spotLight->attachComponent<SpotLightComponnet>(30.0f, 45.0f, glm::vec3{-1.0f, 0.0f, 0.0f});
    spotLight->attachComponent<Transform>(glm::vec3{1.5f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{1.0f, 1.0f, 1.0f});

    singleReg->emplace_or_replace<UBOs>(*spotLight, ubos);

    // 点光源白球相关初始化
    ShaderSrcs whiteShaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\white.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\white.frag"}}};
    this->whiteShader = std::make_shared<Shader>(whiteShaderSrc);
    std::unique_ptr<Mesh> sphereMesh = std::make_unique<Sphere>(2.2f);
    auto materialData = std::make_shared<MaterialData>();
    materialData->appendTexture(textureCache.getTexture(ProgrammedTexture::CHECKERBOARD));
    sphereMesh->setMaterial(materialData);
    sphereEntity = std::make_unique<CommonRenderEntity>(glm::vec3{10.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, glm::vec3{1.0f, 1.0f, 1.0f});
    sphereEntity->attachComponent<IsLight>();

    // 设置网格和shader
    sphereEntity->setMesh(std::move(sphereMesh), whiteShader);

#ifndef NDEBUG
    auto spdlog = RLLogger::getInstance();
    // 检查下是否满足render所需entity
    if (singleReg->all_of<VertArrayComponent, DiscreteUniforms, UBOs, ShaderRef>(*cubeEntity)) {
	spdlog->info("Cube entity has enough components");
    } else {
	spdlog->error("Cube entity has not enough components");
    }
#endif

    // 测试父子关系
    cubeEntity->attachComponent<Relationship>(entt::null, std::vector<entt::entity>{*sphereEntity});

    sphereEntity->attachComponent<Relationship>(*cubeEntity, std::vector<entt::entity>{});  // sphere的父节点是cube
}

void UBOTest::operator()() {
    cam->update();

    singleReg->get<Transform>(*cubeEntity).addRotation(glm::vec3{0.0f, 1.0f, 0.0f});

    // 场景绘制
    CommonRenderEntity::update();
}

UBOTest::~UBOTest() {
    auto renderEntites = singleReg->view<Transform>();
    for (auto entity : renderEntites) {
	singleReg->destroy(entity);
    }
}

LoadModelTest::LoadModelTest(std::shared_ptr<Camera> cam) {
    this->cam = cam;
    ShaderSrcs modelShaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\Light\\phong_ubo.vert"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\Light\\spotlight_ubo.frag"}}};
    this->modelShader = std::make_shared<Shader>(modelShaderSrc);
    directionalLight = std::make_unique<GeneralEntity>();
    // 光源entity
    directionalLight->attachComponent<CommonLight>(glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.1f, 0.2f, 0.2f}, 32.0f);
    directionalLight->attachComponent<Transform>(glm::vec3{-1.5f, 0.0f, -10.0f}, glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{1.0f, 1.0f, 1.0f});
    importer = std::make_unique<ModelImporter>("assest\\bag\\backpack.obj");
    importer->processNodeBFS(modelShader);
    // 检查下CommonRenderEntity数量
    auto commonRenderEntities = singleReg->view<Transform>();
    assert(commonRenderEntities.size() > 0 && "No CommonRenderEntity created");
    auto logger = RLLogger::getInstance();
    logger->debug("CommonRenderEntity count: {}", commonRenderEntities.size());
    logger->debug("Nodes count: {}", importer->getNodeCount());
}

void LoadModelTest::operator()() {
    cam->update();
    CommonRenderEntity::update();
}

LoadModelTest::~LoadModelTest() {
    auto renderEntites = singleReg->view<Transform>();
    for (auto entity : renderEntites) {
	singleReg->destroy(entity);
    }
}

}  // namespace practice
}  // namespace RGL
