#include "CameraECS.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "DataPipeline.hpp"
#include "ControllerECS.hpp"
namespace RGL {


	using namespace entt::literals;


PerspectiveCamSystem::PerspectiveCamSystem(float fovy, float aspect, float nearplan, float farplan) : fovy(fovy), aspect(aspect), mNear(nearplan), mFar(farplan)
{

    proj = glm::perspective(glm::radians(fovy), aspect, mNear, mFar);

}

void PerspectiveCamSystem::update() {
    auto view = singleReg->view<const CameraBasicAttributes, CameraPose, CameraProjection>();
    resizeViewport();
    auto entity = *view.begin();
    const auto& camMove = view.get<CameraBasicAttributes>(entity);
    const auto& camPose = view.get<CameraPose>(entity);
    auto& proj = view.get<CameraProjection>(entity);
    // 透视投影矩阵
    proj.projMat = this->proj;	//(1.15,1.73,0.2)
    // View矩阵
    const glm::vec3 front = glm::cross(camPose.up, camPose.right);  // 右手法则，up×right，得出camera前方向
    const glm::vec3 center = camPose.position + front;		    // 从摄像机位置往前看，就是center
    proj.viewMat = glm::lookAt(camPose.position, center, camPose.up);
   
	singleReg->ctx().insert_or_assign("CameraProjection"_hs, proj);
	singleReg->ctx().insert_or_assign("cameraPos"_hs, camPose.position);
}
void PerspectiveCamSystem::resizeViewport() {
    auto sharingData = SharingData::getInstance();
    auto resizedWindow = sharingData->getData("resizedWindow");
    if (!resizedWindow.data()) {
        return;
    }

    auto windowResizeEvent = entt::any_cast<WindowResizeEvent>(resizedWindow);

    if (windowResizeEvent.handled) {
	return;
    }
    windowResizeEvent.handled = true;

    glcore::glCall(glViewport, 0, 0, windowResizeEvent.newwidth, windowResizeEvent.newheight);
    aspect = static_cast<float>(windowResizeEvent.newwidth) / static_cast<float>(windowResizeEvent.newheight);
    proj = glm::perspective(glm::radians(fovy), aspect, mNear, mFar);
}
}  // namespace RGL
