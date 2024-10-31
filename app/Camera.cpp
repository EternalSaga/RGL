#include "Camera.hpp"
#include <glm/ext/quaternion_geometric.hpp>
#include "EnttRegistry.hpp"
namespace RGL {

Camera::Camera() : position(0.0f, 0.0f, 1.0f),
		   up(0.0f, 1.0f, 0.0f),
		   right(1.0f, 0.0f, 0.0f) {
}
glm::mat4
Camera::getViewMatrix() const {
    const glm::vec3 front = glm::cross(up, right);  // 右手法则，up×right，得出camera前方向

    const glm::vec3 center = position + front;	// 从摄像机位置往前看，就是center

    return glm::lookAt(position, center, up);
}
OrthographicCamera::OrthographicCamera(float l, float r, float t, float b, float n, float f) : left(l), right(r), top(t), bottom(b), near(n), far(f) {
}
glm::mat4
OrthographicCamera::getProjectionMatrix() const {
    return glm::ortho(left, right, bottom, top, near, far);
}
void OrthographicCamera::scale(float deltaScale) {
}
PerspectiveCamera::PerspectiveCamera(float fovy, float aspect, float near, float far) : fovy(fovy), aspect(aspect), near(near), far(far) {
}
glm::mat4
PerspectiveCamera::getProjectionMatrix() const {
    return glm::perspective(glm::radians(fovy), aspect, near, far);
}
void PerspectiveCamera::scale(float deltaScale) {
    const auto front = glm::cross(this->up, this->right);
    this->position += front * deltaScale;
}

}  // namespace RGL
