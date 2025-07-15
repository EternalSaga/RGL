#include "Shader.hpp"
//#include "grass_fragment_shader_template.hpp"
#include "EnttRegistry.hpp"
#include "UBO.hpp"
#include "SSBO.hpp"

namespace RGL {
namespace glcore {
class InstancedGrassShaderSystem : SingleReg{
    void setupShader() {
        // Render logic here
        // auto view = singleReg->view<grass_fragment::InstanceData,grass_fragment::CameraBlock,grass_fragment::DirectionLight,UBO,SSBO,ShaderRef>();
        // view.each([](auto entity, auto instanceData,auto cameraBlock,auto directionLight,auto ubo,auto ssbo,auto shaderRef) {
            
            
        // });

    }
};

}  // namespace render_core
}  // namespace RGL