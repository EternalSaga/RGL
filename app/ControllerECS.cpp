#include "ControllerECS.hpp"
#include "CameraECS.hpp"
#include <SDL2/SDL.h>
#include "CameraController.hpp"
#include <glm/gtc/matrix_transform.hpp>
namespace RGL {



TrackBallMouseKeyboardSystem::TrackBallMouseKeyboardSystem() {
    singleReg = EnttReg::getPrimaryRegistry();
}
void TrackBallMouseKeyboardSystem::update() {
    auto view = singleReg->view<SDL_Event, MouseKeyboardControlComponent, const CamMovmentComponent, CameraEulerAngle>();
    for (auto entity : view) {
	auto &sdlevent = view.get<SDL_Event>(entity);
	SDL_PollEvent(&sdlevent);
	auto &mouseKeyboard = view.get<MouseKeyboardControlComponent>(entity);
	const auto &camMove = view.get<CamMovmentComponent>(entity);
	auto &eularAngle = view.get<CameraEulerAngle>(entity);
	if (sdlevent.type == SDL_QUIT) {
	    mouseKeyboard.shouldQuit = true;
	} else if (sdlevent.type == SDL_WINDOWEVENT) {
	    if (sdlevent.window.event == SDL_WINDOWEVENT_RESIZED) {
		// to do
	    }
	} else if (sdlevent.type == SDL_MOUSEBUTTONDOWN) {
	    const SDL_MouseButtonEvent &mouseEvt = sdlevent.button;
	    mouseKeyboard.leftDown = mouseEvt.button == SDL_BUTTON_LEFT;
	    mouseKeyboard.rightDown = mouseEvt.button == SDL_BUTTON_RIGHT;
	    mouseKeyboard.middleDown = mouseEvt.button == SDL_BUTTON_MIDDLE;
	    if (mouseKeyboard.leftDown) {
		mouseKeyboard.leftdownCursor = glm::vec2(mouseEvt.x, mouseEvt.y);
	    }
	} else if (sdlevent.type == SDL_MOUSEBUTTONUP) {
	    const SDL_MouseButtonEvent &mouseEvt = sdlevent.button;
	    if (mouseEvt.button == SDL_BUTTON_LEFT) {
		mouseKeyboard.leftDown = false;
	    }
	    if (mouseEvt.button == SDL_BUTTON_RIGHT) {
		mouseKeyboard.rightDown = false;
	    }
	    if (mouseEvt.button == SDL_BUTTON_MIDDLE) {
		mouseKeyboard.middleDown = false;
	    }
	} else if (sdlevent.type == SDL_MOUSEWHEEL) {
	    // 设置camera缩放参数
	    eularAngle.deltaScale = sdlevent.wheel.y;
	} else if (sdlevent.type == SDL_MOUSEMOTION) {
	    const SDL_MouseMotionEvent &cursorEvt = sdlevent.motion;
	    mouseKeyboard.currentCursor = glm::vec2(cursorEvt.x, cursorEvt.y);
	    // 如果鼠标在中间，那么把leftdownCursor更新为lastCursor
	    if (currentCursorBetween(mouseKeyboard.leftdownCursor, mouseKeyboard.lastCursor, mouseKeyboard.currentCursor)) {
		mouseKeyboard.leftdownCursor = mouseKeyboard.lastCursor;
	    } else {
		if (mouseKeyboard.leftDown) {
		    const glm::vec2 draggingDelta = mouseKeyboard.currentCursor - mouseKeyboard.leftdownCursor;
		    const glm::vec2 draggingAngle = camMove.sensitivity * draggingDelta;  // 敏感度需要提前设置
		    eularAngle.pitchAngle = -draggingAngle.y;				  // doPitch(-draggingAngle.y);
		    eularAngle.yawAngle = -draggingAngle.x;				  // doYaw(-draggingAngle.x);
		}
		mouseKeyboard.lastCursor = mouseKeyboard.currentCursor;
	    }
	}
    }
}
TrackballSystem::TrackballSystem() {
    singleReg = EnttReg::getPrimaryRegistry();
}
void TrackballSystem::update() {
    auto view = singleReg->view<const CameraEulerAngle, CameraPose>();
    for (auto entity : view) {
	const auto &eulerAngle = view.get<CameraEulerAngle>(entity);
	auto &camPose = view.get<CameraPose>(entity);

	// pitch
	// 既然是俯仰角度旋转，那么创建的旋转矩阵就是以摄像机的右方向为旋转轴
	const auto pitchRotationMat = glm::rotate(glm::mat4(1.0f), glm::radians(eulerAngle.pitchAngle), camPose.right);
	camPose.up = pitchRotationMat * glm::vec4(camPose.up, 0.0f);
	camPose.position = pitchRotationMat * glm::vec4(camPose.position, 1.0f);

	// yaw
	// 绕世界坐标系y轴横向旋转
	const auto yawRotationMat = glm::rotate(glm::mat4(1.0f), glm::radians(eulerAngle.yawAngle), glm::vec3(0.0f, 1.0f, 0.0f));
	camPose.up = yawRotationMat * glm::vec4(camPose.up, 0.0f);
	camPose.right = yawRotationMat * glm::vec4(camPose.right, 0.0f);
	camPose.position = yawRotationMat * glm::vec4(camPose.position, 1.0f);

	// scale
	const auto front = glm::cross(camPose.up, camPose.right);
	camPose.position += front * eulerAngle.deltaScale;
    }
}
}  // namespace RGL
