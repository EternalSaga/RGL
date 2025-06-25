#include "InstanceComponent.hpp"
#include <random>
#include <glm/gtc/matrix_transform.hpp>
#include "GLCheckError.hpp"
#include "Helpers.hpp"
#include "Mesh.hpp" 
#include "CameraECS.hpp"
#include "ShaderManager.hpp"
#include "Light.hpp"
using namespace entt::literals;
namespace RGL {
namespace glcore {

namespace InstanceFactory {
std::vector<glm::mat4> generateRandomTransforms(
    size_t numInstances, 
    const glm::vec3& minPos, 
    const glm::vec3& maxPos,
    float minScale, 
    float maxScale) 
{
    std::vector<glm::mat4> matrices;
    matrices.reserve(numInstances); // 提前预留空间，避免循环中重分配

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> xDist(minPos.x, maxPos.x);
    std::uniform_real_distribution<float> yDist(minPos.y, maxPos.y);
    std::uniform_real_distribution<float> zDist(minPos.z, maxPos.z);
    std::uniform_real_distribution<float> scaleDist(minScale, maxScale);
    std::uniform_real_distribution<float> rotDist(0.0f, 360.0f);

    for (size_t i = 0; i < numInstances; ++i) {
        glm::mat4 model = glm::mat4(1.0f);
        glm::vec3 position(xDist(rng), yDist(rng), zDist(rng));
        model = glm::translate(model, position);
        model = glm::rotate(model, glm::radians(rotDist(rng)), glm::vec3(0.0f, 1.0f, 0.0f));
        float scale = scaleDist(rng);
        model = glm::scale(model, glm::vec3(scale));
        matrices.push_back(model);
    }

    
    return matrices;
}

InstancedRenderingComponent createComponent(const std::vector<glm::mat4>& instanceMatrices) {
    if (instanceMatrices.empty()) {
        // 处理空数据的情况
        return {nullptr, 0};
    }

    auto instanceVBO = std::make_unique<glcore::VBO>();
    glCall(glNamedBufferData, 
           *instanceVBO, 
           instanceMatrices.size() * sizeof(glm::mat4), 
           instanceMatrices.data(), 
           GL_STATIC_DRAW);

    return { std::move(instanceVBO), instanceMatrices.size() };
}

} // namespace InstanceFactory

void InstancedRenderSystem::update(bool disableCulling) {
    // 获取相机矩阵
    const auto& proj = singleReg->ctx().get<CameraProjection>("CameraProjection"_hs);

    auto view = singleReg->view<const InstancedMeshComponent>();

    view.each([&](const auto entity, const InstancedMeshComponent& instancedMesh) {
	// 1. 绑定Shader
	ScopeShader scopeShader(*instancedMesh.shader);

	// 2. 更新通用Uniforms (相机、光照等)
	instancedMesh.shader->setUniform("viewMatrix", proj.viewMat);
	instancedMesh.shader->setUniform("projectionMatrix", proj.projMat);
	// 这里你还需要调用光照更新的逻辑，比如 updateDirLight()
	updateDirLight();  // 确保光照UBO被更新

	// 3. 绑定纹理

	SamplerCreater::SamplersScope samplerScope(const_cast<SamplerCreater::Samplers&>(instancedMesh.samplers));

	for (const auto& sampler : instancedMesh.samplers) {
	    instancedMesh.shader->setUniform(sampler.samplerName, sampler.textureUnit);
	}

	// 4. 绑定VAO

	VAOScope vaoScope(*instancedMesh.baseMeshVAO);

	if (disableCulling) {
	    glCall(glDisable, GL_CULL_FACE);  // 草地需要双面渲染
	}

	// 5. **执行实例化绘制**
	glCall(glDrawElementsInstanced,
	    GL_TRIANGLES,
	    instancedMesh.indexCount,
	    GL_UNSIGNED_INT,
	    nullptr,  // 索引偏移量为0
	    instancedMesh.instanceCount);

	if (disableCulling) {
	    glCall(glEnable, GL_CULL_FACE);
	}
    });
}
}  // namespace glcore
}  // namespace RGL