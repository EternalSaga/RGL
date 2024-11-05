#include "Camera.hpp"
#include <glm/ext/quaternion_geometric.hpp>
#include "EnttRegistry.hpp"
#include <iostream>
#include "CameraECS.hpp"
#include <cassert>
namespace RGL {

// Camera::Camera() : position(0.0f, 0.0f, 1.0f),
//		   up(0.0f, 1.0f, 0.0f),
//		   right(1.0f, 0.0f, 0.0f) {
// }
// glm::mat4
// Camera::getViewMatrix() const {
//     const glm::vec3 front = glm::cross(up, right);  // 右手法则，up×right，得出camera前方向
//
//     const glm::vec3 center = position + front;	// 从摄像机位置往前看，就是center
//
//     return glm::lookAt(position, center, up);
// }
// OrthographicCamera::OrthographicCamera(float l, float r, float t, float b, float n, float f) : left(l), right(r), top(t), bottom(b), near(n), far(f) {
// }
// glm::mat4
// OrthographicCamera::getProjectionMatrix() const {
//     return glm::ortho(left, right, bottom, top, near, far);
// }
// void OrthographicCamera::scale(float deltaScale) {
// }
// PerspectiveCamera::PerspectiveCamera(float fovy, float aspect, float near, float far) : fovy(fovy), aspect(aspect), near(near), far(far) {
// }
// glm::mat4
// PerspectiveCamera::getProjectionMatrix() const {
//     return glm::perspective(glm::radians(fovy), aspect, near, far);
// }
// void PerspectiveCamera::scale(float deltaScale) {
//     const auto front = glm::cross(this->up, this->right);
//     this->position += front * deltaScale;
// }

PerspectiveTrackballCamera::PerspectiveTrackballCamera(float fovy, float aspect, float near, float far) : cameraSystem(fovy, aspect, near, far), singleReg(EnttReg::getPrimaryRegistry()), mouseKeyboardStatus(singleReg->create()), cameraBasicAttributes(singleReg->create()), cameraPose(singleReg->create()), eulerAngle(singleReg->create()), cameraProjection(singleReg->create()) {
    singleReg->emplace<MouseKeyboardInput>(mouseKeyboardStatus, false, false, false, false, glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.0f}, std::map<int, bool>{});

    // init sensitivity,scale,speed
    singleReg->emplace<CameraBasicAttributes>(cameraBasicAttributes, 0.2f, 0.1f, 0.5f);
    // init position,up vector,right vector
    singleReg->emplace<CameraPose>(cameraPose, glm::vec3{0.0f, 0.0f, 1.0f}, glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{1.0f, 0.0f, 0.0f});

    singleReg->emplace<CameraEulerMoveParams>(eulerAngle, 0.0f, 0.0f, 0.0f, glm::vec3{0.0f, 0.0f, 0.0f});
    singleReg->emplace<CameraProjection>(cameraProjection, glm::mat4{1.0f}, glm::mat4{1.0f});
    auto view = singleReg->view<CameraPose, CameraEulerMoveParams>();

    assert(singleReg->valid(cameraPose));
    assert(singleReg->valid(eulerAngle));
    view.each([](auto pose, auto param) {
	int a = 111;
	std::cout << a;
    });
}

void PerspectiveTrackballCamera::update() {
    trackballMouseKeyboardControl.update();
    trackballSystem.update();
    cameraSystem.update();
}

PerspectiveTrackballCamera::~PerspectiveTrackballCamera() {
    singleReg->destroy(cameraProjection);
    singleReg->destroy(eulerAngle);
    singleReg->destroy(cameraPose);
    singleReg->destroy(cameraBasicAttributes);
    singleReg->destroy(mouseKeyboardStatus);
}

}  // namespace RGL
