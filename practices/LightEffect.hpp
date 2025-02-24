
#pragma once
#include "Camera.hpp"
#include "EnttRegistry.hpp"
#include "GLObj.hpp"
#include "GLTextures.hpp"

#include "Shader.hpp"

#include "api_types.hpp"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <memory>
#include "Light.hpp"
#include "ModelImporter.hpp"
#include "Entity.hpp"
namespace RGL {
namespace practice {

using namespace glcore;



class UBOTest : public Renderer, public SingleReg {
    ShaderRef spotlightShader;
    ShaderRef whiteShader;
    std::shared_ptr<Camera> cam;

	std::shared_ptr<UBO> lightUBO;
    std::shared_ptr<UBO> transformUBO;

    TextureCache textureCache;
	std::shared_ptr<Texture> box_texture;
    
    std::unique_ptr<CommonRenderEntity> cubeEntity;
	std::unique_ptr<GeneralEntity> spotLight;

	std::unique_ptr<CommonRenderEntity> sphereEntity;
	UBOs ubos;

    std::shared_ptr<MaterialData> material;

    
   public:
    UBOTest(std::shared_ptr<Camera> cam);
    void operator()() override;
    ~UBOTest();
};

class LoadModelTest : public Renderer,public SingleReg{

    ShaderRef modelShader;
	std::shared_ptr<Camera> cam;
	std::unique_ptr<CommonRenderEntity> modelEntity;


    std::unique_ptr<GeneralEntity> directionalLight;
    UBOs ubos;

    std::unique_ptr<ModelImporter> importer;
public:
    LoadModelTest(std::shared_ptr<Camera> cam);
    void operator()() override;
    ~LoadModelTest();

};


}  // namespace practice
}  // namespace RGL
