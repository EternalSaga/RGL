#include "CameraECS.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "DataPipeline.hpp"
namespace RGL {
PerspectiveCamSystem::PerspectiveCamSystem(float fovy, float aspect, float nearplan, float farplan) : fovy(fovy), aspect(aspect), mNear(nearplan), mFar(farplan)

{
    singleReg = EnttReg::getPrimaryRegistry();
    proj = glm::perspective(glm::radians(fovy), aspect, mNear, mFar);

    shaderManager = glcore::ShaderManager::getInstance();
}

void PerspectiveCamSystem::update() {
    auto view = singleReg->view<const CameraBasicAttributes, CameraPose, CameraProjection>();

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

    shaderManager->updateUniform("viewMatrix", proj.viewMat);
    shaderManager->updateUniform("projectionMatrix", proj.projMat);
    shaderManager->updateUniform("cameraPos", camPose.position);

	SharingData* sharingData = SharingData::getInstance();
    sharingData->setData("CameraProjection", proj);
}
}  // namespace RGL
