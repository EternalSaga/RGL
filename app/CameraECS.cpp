#include "CameraECS.hpp"
#include <glm/gtc/matrix_transform.hpp>
namespace RGL {
PerspectiveCamSystem::PerspectiveCamSystem(float fovy, float aspect, float nearplan, float farplan) : fovy(fovy), aspect(aspect), near(nearplan), far(farplan)

{
    singleReg = EnttReg::getPrimaryRegistry();
    proj = glm::perspective(glm::radians(fovy), aspect, near, far);
}



ShaderUniformCamProperties PerspectiveCamSystem::update() {
    auto view = singleReg->view<const CameraBasicAttributes, CameraPose, CameraProjection>();

    for (auto entity : view) {
	const auto& camMove = view.get<CameraBasicAttributes>(entity);
	const auto& camPose = view.get<CameraPose>(entity);
	auto& proj = view.get<CameraProjection>(entity);
	// 透视投影矩阵
	proj.projMat = this->proj;  //(1.15,1.73,0.2)
	// View矩阵
	const glm::vec3 front = glm::cross(camPose.up, camPose.right);	// 右手法则，up×right，得出camera前方向
	const glm::vec3 center = camPose.position + front;		// 从摄像机位置往前看，就是center
	proj.viewMat = glm::lookAt(camPose.position, center, camPose.up);

	// 发布数据
	ShaderUniformCamProperties camProperties;
	camProperties.viewMat = proj.viewMat;
	camProperties.projMat = proj.projMat;
	camProperties.position = camPose.position;

	return camProperties;
    }
}

}  // namespace RGL
