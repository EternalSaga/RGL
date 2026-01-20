#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_float3.hpp>
#include <glm/fwd.hpp>
#include <memory>

#include "Entity.hpp"
#include "Geometry.hpp"

#include "Mesh.hpp"

#include "RenderQueue.hpp"
#include "SSBO.hpp"
#include "Shader.hpp"

#include "Material.hpp"

#include "LightEffect.hpp"

#include "rllogger.hpp"
#include "UBO.hpp"

#include "Mesh.hpp"
#define GLM_ENABLE_EXPERIMENTAL

#include <glm/gtx/string_cast.hpp>
#include "GLTextures.hpp"
#include "grass_fragment.hpp"
#include "phong_ubo_instanced.hpp"
#include "InstanceComponent.hpp"

// #include "grass_fragment_shader_template.hpp"
namespace RGL {
namespace practice {
using namespace entt::literals;
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

using namespace phong_ubo_instanced;
using namespace grass_fragment;
LoadModelTest::LoadModelTest(std::shared_ptr<Camera> cam) : renderQueues{} {
    this->cam = cam;

    // Initialize container pointers
    ubos = std::make_shared<UBOs::element_type>();
    ssbos = std::make_shared<SSBOs::element_type>();

    ShaderBytesPath modelShaderSrc = {
	{SHADER_TYPE::VERTEX, {"shaders\\phong_ubo_instanced.spv"}},
	{SHADER_TYPE::FRAGMENT, {"shaders\\grass-fragment.spv"}}};
    grassShader = std::make_shared<Shader>(modelShaderSrc);

    importer = std::make_unique<ModelImporter>("assets\\grass_variations.glb");
    auto singleGrassMesh = importer->importAsSingleMesh();

    auto randomTransforms = InstanceFactory::generateRandomTransforms(
	100,
	glm::vec3(-50.0f, 0.0f, -50.0f), glm::vec3(50.0f, 0.0f, 50.0f),
	0.5f, 1.5f);

    this->grassVAO = VAOCreater::createMeshVAO(*singleGrassMesh, *grassShader);

    auto grassFieldEntity = singleReg->create();

    // Setup UBOs
    ubos->emplace(CameraBlock::getUboName(), std::make_shared<UBO>(CameraBlock::BINDING_POINT, sizeof(phong_ubo_instanced::detail::CameraBlockData)));
    ubos->emplace(DirectionLight::getUboName(), std::make_shared<UBO>(DirectionLight::BINDING_POINT, sizeof(grass_fragment::detail::DirectionLightData)));
    ubos->emplace(MaterialIndices::getUboName(), std::make_shared<UBO>(MaterialIndices::BINDING_POINT, sizeof(grass_fragment::detail::MaterialIndicesData)));

    // Setup SSBOs
    ssbos->emplace(InstanceData::getName(), std::make_shared<SSBO>(InstanceData::BINDING_POINT));

    // Populate SSBO initially (optional if system updates it, but good for safety)
    (*ssbos)[InstanceData::getName()]->updateBuffer(randomTransforms.data(), sizeof(decltype(randomTransforms[0])) * randomTransforms.size());

    Transform transform{glm::vec3{0.0f, 0.0f, 0.0f}};
    auto modelLocalAABB = singleGrassMesh->getAABB();

    const float desiredSize = 10.0f;  // 1.0f约等于1米，所以20.0f约等于20米的草地
    transform.formToAABB(modelLocalAABB, desiredSize);

    // Create and Populate Data Components
    phong_ubo_instanced::InstanceData instanceDataComp(randomTransforms.size());
    std::memcpy(const_cast<void*>(instanceDataComp.data()), randomTransforms.data(), randomTransforms.size() * sizeof(decltype(randomTransforms[0])));

    singleReg->emplace<Transform>(grassFieldEntity, transform);
    auto [vertCount, idxOffset] = singleGrassMesh->getIdicesCountAndOffset();
    singleReg->emplace<VertArrayComponent>(grassFieldEntity, std::move(grassVAO), vertCount, idxOffset);
    singleReg->emplace<UBOs>(grassFieldEntity, ubos);
    singleReg->emplace<SSBOs>(grassFieldEntity, ssbos);
    singleReg->emplace<ShaderRef>(grassFieldEntity, grassShader);
    singleReg->emplace<std::shared_ptr<AssetMaterialData>>(grassFieldEntity, singleGrassMesh->getMaterial());
    singleReg->emplace<RenderTags::Instanced>(grassFieldEntity, 100ull);
    singleReg->emplace<RenderTags::Renderable>(grassFieldEntity);

    // Add components required by InstancedGrassShaderSystem
    singleReg->emplace<phong_ubo_instanced::InstanceData>(grassFieldEntity, std::move(instanceDataComp));
    singleReg->emplace<phong_ubo_instanced::CameraBlock>(grassFieldEntity);
    singleReg->emplace<grass_fragment::DirectionLight>(grassFieldEntity);

    // Set default light
    auto& light = singleReg->get<grass_fragment::DirectionLight>(grassFieldEntity);
    // Assuming standard naming convention from the generator
    light.set_direction(glm::vec3(0.5f, -1.0f, 0.5f));
    light.set_color(glm::vec3(1.0f, 1.0f, 1.0f));

    singleReg->emplace<grass_fragment::MaterialIndices>(grassFieldEntity);

    frameObjectInView(cam, modelLocalAABB);
    const CameraProjection proj = singleReg->ctx().get<CameraProjection>("CameraProjection"_hs);
    const glm::vec3 camPosition = singleReg->ctx().get<glm::vec3>("cameraPos"_hs);
}

void LoadModelTest::operator()() {
    cam->update();

    // Update CameraBlock Component from Camera
    auto view = singleReg->view<phong_ubo_instanced::CameraBlock>();
    for (auto entity : view) {
	auto& cb = view.get<phong_ubo_instanced::CameraBlock>(entity);
	cb.set_view(cam->getViewMatrix());
	cb.set_projection(cam->getProjectionMatrix());
	cb.set_cameraPos(cam->getPos());
    }

    RenderQueueSystem::populateRenderqueues(renderQueues);

    // Update GPU buffers from Components
    instancedGrassShaderSystem.update();

    // Draw
    RenderQueueSystem::processInstanceQueue(renderQueues.instanceQueue);
}

LoadModelTest::~LoadModelTest() {
    auto renderEntites = singleReg->view<Transform>();
    for (auto entity : renderEntites) {
	singleReg->destroy(entity);
    }
}

}  // namespace practice
}  // namespace RGL
