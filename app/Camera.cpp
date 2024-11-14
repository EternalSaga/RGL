#include "Camera.hpp"
#include <glm/ext/quaternion_geometric.hpp>
#include "EnttRegistry.hpp"
#include <iostream>
#include "CameraECS.hpp"
#include <cassert>
namespace RGL {

PerspectiveTrackballCamera::PerspectiveTrackballCamera(float fovy, float aspect, float near, float far) : cameraSystem(fovy, aspect, near, far), singleReg(EnttReg::getPrimaryRegistry()), perspcativeCameraEntity(singleReg->create()) {
    singleReg->emplace<MouseKeyboardInput>(perspcativeCameraEntity, false, false, false, false, glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.0f}, std::map<int, bool>{});

    // init sensitivity,scale,speed
    singleReg->emplace<CameraBasicAttributes>(perspcativeCameraEntity, 0.1f, 0.2f, 0.2f);
    // init position,up vector,right vector
    singleReg->emplace<CameraPose>(perspcativeCameraEntity, glm::vec3{0.0f, 0.0f, 100.0f}, glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{1.0f, 0.0f, 0.0f});

    singleReg->emplace<CameraEulerMoveParams>(perspcativeCameraEntity, 0.0f, 0.0f, 0.0f, glm::vec3{0.0f, 0.0f, 0.0f});
    singleReg->emplace<CameraProjection>(perspcativeCameraEntity, glm::mat4{1.0f}, glm::mat4{1.0f});
}

ShaderUniformCamProperties PerspectiveTrackballCamera::update() {
    trackballMouseKeyboardControl.update();
    trackballSystem.update();
    const auto camProjProperties = cameraSystem.update();
    return camProjProperties;
}

PerspectiveTrackballCamera::~PerspectiveTrackballCamera() {
    singleReg->destroy(perspcativeCameraEntity);
}

}  // namespace RGL
