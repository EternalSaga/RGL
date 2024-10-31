#include "CameraECS.hpp"
#include <glm/gtc/matrix_transform.hpp>
namespace RGL {
PerspectiveCamSystem::PerspectiveCamSystem(float fovy, float aspect, float near, float far) : fovy(fovy), aspect(aspect), near(near), far(far)

{
    singleReg = EnttReg::getPrimaryRegistry();
    proj = glm::perspective(glm::radians(fovy), aspect, near, far);
}
void PerspectiveCamSystem::update() {
    auto view = singleReg->view<const CameraBasicAttributes, CameraPose, CameraProjection>();

    for (auto entity : view) {
	const auto& camMove = view.get<CameraBasicAttributes>(entity);
	const auto& camPose = view.get<CameraPose>(entity);
	auto& proj = view.get<CameraProjection>(entity);
	// 透视投影矩阵
	proj.projMat = this->proj;
	// View矩阵
	const glm::vec3 front = glm::cross(camPose.up, camPose.right);	// 右手法则，up×right，得出camera前方向
	const glm::vec3 center = camPose.position + front;		// 从摄像机位置往前看，就是center
	proj.viewMat = glm::lookAt(camPose.position, center, camPose.up);
    }
}


}  // namespace RGL
