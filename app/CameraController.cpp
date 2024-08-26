#include "CameraController.hpp"
#include <glm/fwd.hpp>
#include <glm/trigonometric.hpp>
#include <iostream>

namespace RGL
{

void
CamControlGame::doPitch(float angle)
{
    pitch += angle;
    if (pitch > 89.0f || pitch < -89.0f) {
	pitch -= angle;
	return;
    }
    glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(angle), glm::vec3(1.0f, 0.0f, 0.0f));
}
void
CamControlGame::doYaw(float angle)
{
}
void
CamControlGame::onKeyboardDownCbk(const SDL_KeyboardEvent &keyboardEvt)
{
    keyMap[keyboardEvt.keysym.scancode] = true;
}
void
CamControlGame::onKeyboardUpCbk(const SDL_KeyboardEvent &keyboardEvt)
{
    keyMap[keyboardEvt.keysym.scancode] = false;
}
void
CamControlGame::onMouseDownCbk(const SDL_MouseButtonEvent &mouseEvt)
{
    leftDown = mouseEvt.button == SDL_BUTTON_LEFT;
    rightDown = mouseEvt.button == SDL_BUTTON_RIGHT;
    middleDown = mouseEvt.button == SDL_BUTTON_MIDDLE;
}
void
CamControlGame::onMouseUpCbk(const SDL_MouseButtonEvent &mouseEvt)
{
    leftDown = mouseEvt.button != SDL_BUTTON_LEFT;
    rightDown = mouseEvt.button != SDL_BUTTON_RIGHT;
    middleDown = mouseEvt.button != SDL_BUTTON_MIDDLE;
}
void
CamControlGame::onMouseMoveCbk(const SDL_MouseMotionEvent &mouseEvt)
{
    currentCursor = glm::vec2(mouseEvt.x, mouseEvt.y);
}
void
CamControlGame::onMouseWheelCbk(const SDL_MouseWheelEvent &wheelEvt)
{
}
void
CamControlGame::onWindowResizeCbk(const SDL_WindowEvent &windowEvt)
{
}
void
CamControlGame::onCursorMoveCbk(const SDL_MouseMotionEvent &cursorEvt)
{
}
void
CamControlTrackball::onKeyboardDownCbk(const SDL_KeyboardEvent &keyboardEvt)
{
}
void
CamControlTrackball::onKeyboardUpCbk(const SDL_KeyboardEvent &keyboardEvt)
{
}
void
CamControlTrackball::onMouseDownCbk(const SDL_MouseButtonEvent &mouseEvt)
{
    leftDown = mouseEvt.button == SDL_BUTTON_LEFT;
    rightDown = mouseEvt.button == SDL_BUTTON_RIGHT;
    middleDown = mouseEvt.button == SDL_BUTTON_MIDDLE;

}
void
CamControlTrackball::onMouseUpCbk(const SDL_MouseButtonEvent &mouseEvt)
{
    leftDown = mouseEvt.button != SDL_BUTTON_LEFT;
    rightDown = mouseEvt.button != SDL_BUTTON_RIGHT;
    middleDown = mouseEvt.button != SDL_BUTTON_MIDDLE;
}
void
CamControlTrackball::onMouseMoveCbk(const SDL_MouseMotionEvent &mouseEvt)
{
}
void
CamControlTrackball::onMouseWheelCbk(const SDL_MouseWheelEvent &wheelEvt)
{
}
void
CamControlTrackball::onWindowResizeCbk(const SDL_WindowEvent &windowEvt)
{
}

void CamControlTrackball::doPitch(float angle){
    //既然是俯仰角度旋转，那么创建的旋转矩阵就是以摄像机的右方向为旋转轴
    const auto rotationMat = glm::rotate(glm::mat4(1.0f), glm::radians(angle), camera->right);
    camera->up = rotationMat * glm::vec4(camera->up, 0.0f);
    camera->position = rotationMat * glm::vec4(camera->position, 1.0f);
}

void CamControlTrackball::doYaw(float angle){
    //绕世界坐标系y轴横向旋转
    const auto rotationMat = glm::rotate(glm::mat4(1.0f), glm::radians(angle),glm::vec3(0.0f,1.0f,0.0f));
    camera->up = rotationMat * glm::vec4(camera->up, 0.0f);
    camera->right = rotationMat * glm::vec4(camera->right, 0.0f);
    camera->position = rotationMat * glm::vec4(camera->position, 1.0f);
}

void
CamControlTrackball::onCursorMoveCbk(const SDL_MouseMotionEvent &cursorEvt)
{
    currentCursor = glm::vec2(cursorEvt.x, cursorEvt.y);
    if (leftDown) {
	    const glm::vec2 draggingDelta = currentCursor - previousCursor;
        glm::vec2 draggingAngle = sensitivity * draggingDelta;
        doPitch(-draggingAngle.y);
        doYaw(-draggingAngle.x);
    }
}
} // namespace RGL