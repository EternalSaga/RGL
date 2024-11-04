#include "EnttRegistry.hpp"
#include <Shader.hpp>
#include <memory>
#include <CameraECS.hpp>
namespace RGL {
namespace glcore {

class CameraUniformSystem : public SingleReg {
    std::shared_ptr<Shader> shaderPtr;
   public:
    CameraUniformSystem(std::shared_ptr<Shader> shader) : shaderPtr(shader) {
    }
    void update() {
	auto view = singleReg->view<CameraProjection, CameraPose>();
	for (auto entity : view) {
	    const auto& proj = view.get<CameraProjection>(entity);
	    shaderPtr->setUniformMat("projectionMatrix", proj.projMat);
	    shaderPtr->setUniformMat("viewMatrix", proj.viewMat);
	    const auto& camPose = view.get<CameraPose>(entity);
	    shaderPtr->setUniform("cameraPos", camPose.position);
    }
};

}  // namespace glcore
}  // namespace RGL
