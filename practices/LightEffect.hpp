
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

};


}  // namespace practice
}  // namespace RGL
