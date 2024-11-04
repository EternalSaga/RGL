#include "ControllerECS.hpp"
#include "CameraECS.hpp"
#include <SDL2/SDL.h>
#include "CameraController.hpp"
#include <glm/gtc/matrix_transform.hpp>
namespace RGL {



class GameMouseKeyboardSystem : public SingleReg {
   public:
    void update() {
	auto view = singleReg->view<MouseKeyboardInput, const CameraBasicAttributes, CameraEulerMoveParams>();
	for (auto entity : view) {
	    SDL_Event sdlevent;
	    SDL_PollEvent(&sdlevent);
	    auto &mouseKeyboard = view.get<MouseKeyboardInput>(entity);
	    const auto &camAttributes = view.get<CameraBasicAttributes>(entity);
	    auto &eularMove = view.get<CameraEulerMoveParams>(entity);

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

		if (mouseKeyboard.rightDown) {
		    mouseKeyboard.rightdownCursor = glm::vec2(mouseEvt.x, mouseEvt.y);
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
		} else if (sdlevent.type == SDL_MOUSEWHEEL) {
		    eularMove.deltaScale = sdlevent.wheel.y;

		} else if (sdlevent.type == SDL_MOUSEMOTION) {
		    const SDL_MouseMotionEvent &cursorEvt = sdlevent.motion;

		    mouseKeyboard.currentCursor = glm::vec2(cursorEvt.x, cursorEvt.y);

		    // 如果鼠标在中间，那么把leftdownCursor更新为lastCursor
		    if (currentCursorBetween(mouseKeyboard.rightdownCursor, mouseKeyboard.lastCursor, mouseKeyboard.currentCursor)) {
			mouseKeyboard.rightdownCursor = mouseKeyboard.lastCursor;

		    } else {
			if (mouseKeyboard.rightDown) {
			    const glm::vec2 draggingDelta = mouseKeyboard.currentCursor - mouseKeyboard.rightdownCursor;

			    const glm::vec2 draggingAngle = camAttributes.sensitivity * draggingDelta;

			    eularMove.pitchAngle = -draggingAngle.y;
			    eularMove.yawAngle = -draggingAngle.x;
			}
			mouseKeyboard.lastCursor = mouseKeyboard.currentCursor;
		    }

		} else if (sdlevent.type == SDL_KEYDOWN) {
		    const SDL_KeyboardEvent &keyboardEvt = sdlevent.key;
		    mouseKeyboard.keyMap[keyboardEvt.keysym.scancode] = true;

		    eularMove.direction = glm::vec3{0.0f, 0.0f, 0.0f};

		    if (mouseKeyboard.keyMap[SDL_SCANCODE_W]) {
			eularMove.direction = glm::vec3{0.0f, 1.0f, 0.0f};
		    }
		    if (mouseKeyboard.keyMap[SDL_SCANCODE_S]) {
			eularMove.direction = glm::vec3{0.0f, -1.0f, 0.0f};
		    }
		    if (mouseKeyboard.keyMap[SDL_SCANCODE_A]) {
			eularMove.direction = glm::vec3{-1.0f, 0.0f, 0.0f};
		    }
		    if (mouseKeyboard.keyMap[SDL_SCANCODE_D]) {
			eularMove.direction = glm::vec3{1.0f, 0.0f, 0.0f};
		    }
		    if (mouseKeyboard.keyMap[SDL_SCANCODE_ESCAPE]) {
		    }
		} else if (sdlevent.type == SDL_KEYUP) {
		    const SDL_KeyboardEvent &keyboardEvt = sdlevent.key;
		    mouseKeyboard.keyMap[keyboardEvt.keysym.scancode] = false;
		}
	    }
	}
    }
};


void TrackBallMouseKeyboardSystem::update() {
    auto view = singleReg->view<SDL_Event, MouseKeyboardInput, const CameraBasicAttributes, CameraEulerMoveParams>();
    for (auto entity : view) {
	auto &sdlevent = view.get<SDL_Event>(entity);
	SDL_PollEvent(&sdlevent);
	auto &mouseKeyboard = view.get<MouseKeyboardInput>(entity);
	const auto &camMove = view.get<CameraBasicAttributes>(entity);
	auto &eularAngle = view.get<CameraEulerMoveParams>(entity);
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

void TrackballSystem::update() {
    auto view = singleReg->view<const CameraEulerMoveParams, CameraPose>();
    for (auto entity : view) {
	const auto &eulerAngle = view.get<CameraEulerMoveParams>(entity);
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

void GameControlSystem::update() {
    auto view = singleReg->view<const CameraEulerMoveParams, CameraPose>();
    for (auto entity : view) {
	const auto &eulerAngle = view.get<CameraEulerMoveParams>(entity);
	auto &camPose = view.get<CameraPose>(entity);

	// pitch
	camPose.pitch += eulerAngle.pitchAngle;
	if (camPose.pitch > 89.0f || camPose.pitch < -89.0f) {
	    camPose.pitch -= eulerAngle.pitchAngle;
	    
	} else {
	    // 在gameCameraControl的情况下，pitch不会影响mPosition
	    // 以世界坐标系的x轴为中心旋转
	    const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(eulerAngle.pitchAngle), glm::vec3(1.0f, 0.0f, 0.0f));
	    // CamControlInner
	    camPose.up = rotation * glm::vec4(camPose.up, 0.0f);
	}


	// yaw
	const auto mat = glm::rotate(glm::mat4(1.0f), glm::radians(eulerAngle.yawAngle), glm::vec3(0.0f, 1.0f, 0.0f));
	camPose.up = mat * glm::vec4(camPose.up, 0.0f);
	camPose.right = mat * glm::vec4(camPose.right, 0.0f);

	// scale
	const auto front = glm::cross(camPose.up, camPose.right);
	camPose.position += front * eulerAngle.deltaScale;
    }
}

}  // namespace RGL
