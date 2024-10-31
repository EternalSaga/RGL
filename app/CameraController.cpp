#include "CameraController.hpp"
#include <SDL_scancode.h>
#include <glm/fwd.hpp>
#include <glm/gtc/matrix_transform.hpp>


namespace RGL {
//void CamControlGame::doPitch(float angle) {
//    pitch += angle;
//    if (pitch > 89.0f || pitch < -89.0f) {
//	pitch -= angle;
//	return;
//    }
//    // 在gameCameraControl的情况下，pitch不会影响mPosition
//    // 以世界坐标系的x轴为中心旋转
//    const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(angle), glm::vec3(1.0f, 0.0f, 0.0f));
//    // CamControlInner
//    camera->up = rotation * glm::vec4(camera->up, 0.0f);
//}
//void CamControlGame::doYaw(float angle) {
//    const auto mat = glm::rotate(glm::mat4(1.0f), glm::radians(angle), glm::vec3(0.0f, 1.0f, 0.0f));
//    camera->up = mat * glm::vec4(camera->up, 0.0f);
//    camera->right = mat * glm::vec4(camera->right, 0.0f);
//}
//void CamControlGame::onKeyboardDownCbk(const SDL_KeyboardEvent &keyboardEvt) {
//    keyMap[keyboardEvt.keysym.scancode] = true;
//    glm::vec3 direction = glm::vec3(0.0f);
//    const auto front = glm::cross(camera->up, camera->right);
//    const auto right = camera->right;
//    if (keyMap[SDL_SCANCODE_W]) {
//	direction += front;
//    }
//    if (keyMap[SDL_SCANCODE_S]) {
//	direction -= front;
//    }
//    if (keyMap[SDL_SCANCODE_A]) {
//	direction -= right;
//    }
//    if (keyMap[SDL_SCANCODE_D]) {
//	direction += right;
//    }
//
//    if (!keyMap[SDL_SCANCODE_ESCAPE]) {
//	if (glm::length(direction) != 0) {
//	    direction = glm::normalize(direction);
//	    camera->position += direction * speed;
//	}
//    } else {
//    }
//}
//void CamControlGame::onKeyboardUpCbk(const SDL_KeyboardEvent &keyboardEvt) {
//    keyMap[keyboardEvt.keysym.scancode] = false;
//}
//void CamControlGame::onMouseDownCbk(const SDL_MouseButtonEvent &mouseEvt) {
//    leftDown = mouseEvt.button == SDL_BUTTON_LEFT;
//    rightDown = mouseEvt.button == SDL_BUTTON_RIGHT;
//    middleDown = mouseEvt.button == SDL_BUTTON_MIDDLE;
//
//    if (rightDown) {
//	rightdownCursor = glm::vec2(mouseEvt.x, mouseEvt.y);
//    }
//}
//void CamControlGame::onMouseUpCbk(const SDL_MouseButtonEvent &mouseEvt) {
//    if (mouseEvt.button == SDL_BUTTON_LEFT) {
//	leftDown = false;
//    }
//    if (mouseEvt.button == SDL_BUTTON_RIGHT) {
//	rightDown = false;
//    }
//    if (mouseEvt.button == SDL_BUTTON_MIDDLE) {
//	middleDown = false;
//    }
//}
//void CamControlGame::onMouseMoveCbk(const SDL_MouseMotionEvent &mouseEvt) {
//}
//void CamControlGame::onMouseWheelCbk(const SDL_MouseWheelEvent &wheelEvt) {
//}
//void CamControlGame::onWindowResizeCbk(const SDL_WindowEvent &windowEvt) {
//}
//void CamControlGame::onCursorMoveCbk(const SDL_MouseMotionEvent &cursorEvt) {
//    currentCursor = glm::vec2(cursorEvt.x, cursorEvt.y);
//
//    // 如果鼠标在中间，那么把leftdownCursor更新为lastCursor
//    if (currentCursorBetween(rightdownCursor, lastCursor, currentCursor)) {
//	rightdownCursor = lastCursor;
//	return;
//    }
//
//    if (rightDown) {
//	const glm::vec2 draggingDelta = currentCursor - rightdownCursor;
//
//	const glm::vec2 draggingAngle = sensitivity * draggingDelta;
//
//	doPitch(-draggingAngle.y);
//	doYaw(-draggingAngle.x);
//    }
//    lastCursor = currentCursor;
//}
//void CamControlTrackball::onKeyboardDownCbk(const SDL_KeyboardEvent &keyboardEvt) {
//}
//void CamControlTrackball::onKeyboardUpCbk(const SDL_KeyboardEvent &keyboardEvt) {
//}
//void CamControlTrackball::onMouseDownCbk(const SDL_MouseButtonEvent &mouseEvt) {
//    leftDown = mouseEvt.button == SDL_BUTTON_LEFT;
//    rightDown = mouseEvt.button == SDL_BUTTON_RIGHT;
//    middleDown = mouseEvt.button == SDL_BUTTON_MIDDLE;
//
//    if (leftDown) {
//	leftdownCursor = glm::vec2(mouseEvt.x, mouseEvt.y);
//    }
//}
//void CamControlTrackball::onMouseUpCbk(const SDL_MouseButtonEvent &mouseEvt) {
//    if (mouseEvt.button == SDL_BUTTON_LEFT) {
//	leftDown = false;
//    }
//    if (mouseEvt.button == SDL_BUTTON_RIGHT) {
//	rightDown = false;
//    }
//    if (mouseEvt.button == SDL_BUTTON_MIDDLE) {
//	middleDown = false;
//    }
//}
//void CamControlTrackball::onMouseMoveCbk(const SDL_MouseMotionEvent &mouseEvt) {
//}
//void CamControlTrackball::onMouseWheelCbk(const SDL_MouseWheelEvent &wheelEvt) {
//    this->camera->scale(wheelEvt.y);
//}
//void CamControlTrackball::onWindowResizeCbk(const SDL_WindowEvent &windowEvt) {
//}
//
//void CamControlTrackball::doPitch(float angle) {
//    // 既然是俯仰角度旋转，那么创建的旋转矩阵就是以摄像机的右方向为旋转轴
//    const auto rotationMat = glm::rotate(glm::mat4(1.0f), glm::radians(angle), camera->right);
//    camera->up = rotationMat * glm::vec4(camera->up, 0.0f);
//    camera->position = rotationMat * glm::vec4(camera->position, 1.0f);
//}
//
//void CamControlTrackball::doYaw(float angle) {
//    // 绕世界坐标系y轴横向旋转
//    const auto rotationMat = glm::rotate(glm::mat4(1.0f), glm::radians(angle), glm::vec3(0.0f, 1.0f, 0.0f));
//    camera->up = rotationMat * glm::vec4(camera->up, 0.0f);
//    camera->right = rotationMat * glm::vec4(camera->right, 0.0f);
//    camera->position = rotationMat * glm::vec4(camera->position, 1.0f);
//}
//
//void CamControlTrackball::onCursorMoveCbk(const SDL_MouseMotionEvent &cursorEvt) {
//    currentCursor = glm::vec2(cursorEvt.x, cursorEvt.y);
//    // 如果鼠标在中间，那么把leftdownCursor更新为lastCursor
//    if (currentCursorBetween(leftdownCursor, lastCursor, currentCursor)) {
//	leftdownCursor = lastCursor;
//	return;
//    }
//
//    if (leftDown) {
//	const glm::vec2 draggingDelta = currentCursor - leftdownCursor;
//
//	const glm::vec2 draggingAngle = sensitivity * draggingDelta;
//
//	doPitch(-draggingAngle.y);
//	doYaw(-draggingAngle.x);
//    }
//    lastCursor = currentCursor;
//}
//CamControlGame::CamControlGame() {
//}
//CamControlInner::CamControlInner() : leftDown(false), rightDown(false), middleDown(false), currentCursor(0.0f, 0.0f), pitch(0.0f), yaw(0.0f), leftdownCursor(0.0f, 0.0f), rightdownCursor(0.0f, 0.0f), lastCursor(0.0f, 0.0f), sensitivity(0.1f), scaleSpeed(0.2f), speed(0.2f) {}



class GameCamSystem {
    entt::registry *singleReg;

   public:
    GameCamSystem() {
	singleReg = EnttReg::getPrimaryRegistry();
    }
    void update() {
    }
};

}  // namespace RGL
