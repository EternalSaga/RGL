#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/fwd.hpp>
#include <memory>

#include "Entity.hpp"
#include "Geometry.hpp"
#include "Light.hpp"
#include "Mesh.hpp"
#include "PointLight.hpp"
#include "RenderQueue.hpp"
#include "Shader.hpp"
#include "SpotLight.hpp"
#include "Material.hpp"

#include "LightEffect.hpp"
#include <winuser.h>
#include "ShaderManager.hpp"
#include "rllogger.hpp"
#include "UBO.hpp"

#include "Mesh.hpp"
#define GLM_ENABLE_EXPERIMENTAL

#include <glm/gtx/string_cast.hpp>

namespace RGL {
namespace practice {

UBOTest::UBOTest(std::shared_ptr<Camera> cam) {
    this->cam = cam;
    ShaderBytesPath pointLightshaderSrc = {
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
    cubeEntity->attachComponent<RenderTags::Renderable>();

    spotLight = std::make_unique<GeneralEntity>();
    // 光源entity
    spotLight->attachComponent<CommonLight>(glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.1f, 0.2f, 0.2f}, 32.0f);
    spotLight->attachComponent<SpotLightComponnet>(30.0f, 45.0f, glm::vec3{-1.0f, 0.0f, 0.0f});
    spotLight->attachComponent<Transform>(glm::vec3{1.5f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{1.0f, 1.0f, 1.0f});

    singleReg->emplace_or_replace<UBOs>(*spotLight, ubos);

    // 点光源白球相关初始化
    ShaderBytesPath whiteShaderSrc = {
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

void frameObjectInView(std::shared_ptr<Camera> cam, const RGL::glcore::AABB& worldAABB) {
    auto singleReg = RGL::EnttReg::getPrimaryRegistry();
    auto view = singleReg->view<RGL::CameraPose, RGL::CameraEulerMoveParams>();	 // 假设相机只有一个实体
    auto cameraEntity = *view.begin();

    auto& camPose = view.get<RGL::CameraPose>(cameraEntity);

    float fovy = 60.0f;
    float aspect = 720.0f / 480.0f;

    glm::vec3 objectCenter = worldAABB.getCenter();
    float objectRadius = glm::length(worldAABB.getSize()) / 2.0f;

    // 根据垂直FOV计算距离
    float distance = objectRadius / tan(glm::radians(fovy / 2.0f));

    // 如果水平方向更宽，则根据水平FOV调整距离
    float horizontalFov = glm::degrees(2.0f * atan(tan(glm::radians(fovy / 2.0f)) * aspect));
    float neededDistHorizontal = (objectRadius / tan(glm::radians(horizontalFov / 2.0f))) * aspect;  // 粗略估算

    distance = glm::max(distance, neededDistHorizontal) * 1.5f;	 // 乘以一个系数，留出一些边距

    // 将相机放在物体中心后面一段距离的位置
    camPose.position = objectCenter + glm::vec3(0.0f, 0.5f, 1.0f) * distance;  // 从斜上方看

    // 相机朝向物体中心 (lookAt逻辑在你的`PerspectiveCamSystem::update`中处理)
    // 我们需要更新up和right来确保相机正确朝向
    glm::vec3 front = glm::normalize(objectCenter - camPose.position);
    camPose.right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
    camPose.up = glm::normalize(glm::cross(camPose.right, front));
}

LoadModelTest::LoadModelTest(std::shared_ptr<Camera> cam) : renderQueues{} {
    this->cam = cam;

    // --- 1. 准备基础网格 ---
    // a. 创建草地Shader
    ShaderBytesPath modelShaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\phong_ubo_instanced.spv"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\grass-fragment.spv"}}};
    grassShader = std::make_shared<Shader>(modelShaderSrc);

    // b. 加载并合并模型为一个基础网格
    importer = std::make_unique<ModelImporter>("assets\\grass_variations.glb");
    auto singleGrassMesh = importer->importAsSingleMesh();

    auto randomTransforms = InstanceFactory::generateRandomTransforms(
	100,
	glm::vec3(-50.0f, 0.0f, -50.0f), glm::vec3(50.0f, 0.0f, 50.0f),
	0.5f, 1.5f);

    this->grassVAO = VAOCreater::createMeshVAO(*singleGrassMesh, randomTransforms, *grassShader);

    directionalLight = std::make_unique<GeneralEntity>();
    directionalLight->attachComponent<CommonLight>(glm::vec3{1.0f, 0.9f, 0.9f}, glm::vec3{0.2f, 0.2f, 0.2f}, 32.0f);
    directionalLight->attachComponent<DirectionalCompnent>(glm::vec3{1.0f, -1.0f, -1.0f});  // 把光照方向往下调一点，效果更好

    lightUBO = std::make_shared<UBO>(*grassShader, "DirectionLight");

    cameraUBO = std::make_shared<UBO>(*grassShader, "CameraBlock");

    ubos = std::make_shared<std::unordered_map<std::string, std::shared_ptr<UBO>>>();
    (*ubos)[lightUBO->getUboName()] = lightUBO;
    (*ubos)[cameraUBO->getUboName()] = cameraUBO;

    directionalLight->attachComponent<UBOs>(ubos);

    auto grassFieldEntity = singleReg->create();
    singleReg->emplace_or_replace<UBOs>(grassFieldEntity, ubos);

    Transform transform{glm::vec3{0.0f, 0.0f, 0.0f}};
    auto modelLocalAABB = singleGrassMesh->getAABB();

    const float desiredSize = 10.0f;  // 1.0f约等于1米，所以20.0f约等于20米的草地
    transform.formToAABB(modelLocalAABB, desiredSize);

    singleReg->emplace<Transform>(grassFieldEntity, transform);
    auto [vertCount, idxOffset] = singleGrassMesh->getIdicesCountAndOffset();
    singleReg->emplace<VertArrayComponent>(grassFieldEntity, std::move(grassVAO), vertCount, idxOffset);
    singleReg->emplace<ShaderRef>(grassFieldEntity, grassShader);

    auto samplers = SamplerCreater::createSamplers(*singleGrassMesh, *grassShader);

    singleReg->emplace<SamplerCreater::Samplers>(grassFieldEntity, samplers);

    singleReg->emplace<RenderTags::Instanced>(grassFieldEntity, 100ull);
    singleReg->emplace<RenderTags::Renderable>(grassFieldEntity);

    frameObjectInView(cam, modelLocalAABB);
}

void LoadModelTest::operator()() {
    cam->update();
    updateDirLight();
    RenderQueueSystem::populateRenderqueues(renderQueues);
    RenderQueueSystem::processInstanceQueue(renderQueues.instanceQueue);
}

LoadModelTest::~LoadModelTest() {
    auto renderEntites = singleReg->view<Transform>();
    for (auto entity : renderEntites) {
	singleReg->destroy(entity);
    }
    auto lightEntities = singleReg->view<DirectionalCompnent>();
    for (auto entity : lightEntities) {
	singleReg->destroy(entity);
    }
}

}  // namespace practice
}  // namespace RGL
