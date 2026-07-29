
#pragma once
#include "Camera.hpp"
#include "EnttRegistry.hpp"
#include "GLObj.hpp"
#include "GLTextures.hpp"

#include "Shader.hpp"

#include "UBO.hpp"
#include "api_types.hpp"
#include <glad/glad.h>
#include <cstddef>
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <memory>
#include "SSBO.hpp"

#include "ModelImporter.hpp"
#include "Entity.hpp"
#include "grass_shader_system.hpp"
namespace RGL {
namespace practice {

using namespace glcore;

class LoadModelTest : public Renderer, public SingleReg {
    ShaderRef grassShader;
    std::shared_ptr<Camera> cam;
    std::unique_ptr<CommonRenderEntity> modelEntity;

    UBOs ubos;
    SSBOs ssbos;
    std::shared_ptr<UBO> lightUBO;

    std::shared_ptr<UBO> cameraUBO;

    std::unique_ptr<ModelImporter> importer;

    std::unique_ptr<VAO> grassVAO;

    RenderQueues renderQueues;
    InstancedGrassShaderSystem instancedGrassShaderSystem;

   public:
    LoadModelTest(std::shared_ptr<Camera> cam);
    void operator()() override;
    ~LoadModelTest();
};

}  // namespace practice
}  // namespace RGL
