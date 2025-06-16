#include "ControllerECS.hpp"
#include "CameraECS.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include "DataPipeline.hpp"

namespace RGL {

	bool currentCursorBetween(const glm::vec2 &downCursor, const glm::vec2 &lastCursor, const glm::vec2 &currentCursor) {
    // 如果鼠标在左键点下时候的点和上一个更新的点的中间，那么中间点和两边点的夹角应该是钝角，点乘小于0
    return glm::dot(currentCursor - lastCursor, currentCursor - downCursor) < 0;
}

	bool ShouldQuit() {
    bool shouldQuit = false;
    const auto singleReg = EnttReg::getPrimaryRegistry();
	    auto view = singleReg->view<const MouseKeyboardInput>();
    for (auto entity : view) {
		auto &mouseKeyboard = view.get<MouseKeyboardInput>(entity);
	if (mouseKeyboard.shouldQuit) {
		    shouldQuit = true;
	    break;
	}
    }
    return shouldQuit;
	}

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
		
	    if (sdlevent.type == SDL_EVENT_QUIT) {
		mouseKeyboard.shouldQuit = true;
	    } else if (sdlevent.type == SDL_EVENT_WINDOW_RESIZED) {
					auto sharingData = SharingData::getInstance();
		const WindowResizeEvent windowResizeEvent{
		    sdlevent.window.data1,
		    sdlevent.window.data2,false};
		sharingData->setData("resizedWindow", windowResizeEvent);
	    } else if (sdlevent.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
		const SDL_MouseButtonEvent &mouseEvt = sdlevent.button;
		mouseKeyboard.leftDown = mouseEvt.button == SDL_BUTTON_LEFT;
		mouseKeyboard.rightDown = mouseEvt.button == SDL_BUTTON_RIGHT;
		mouseKeyboard.middleDown = mouseEvt.button == SDL_BUTTON_MIDDLE;

		if (mouseKeyboard.rightDown) {
		    mouseKeyboard.rightdownCursor = glm::vec2(mouseEvt.x, mouseEvt.y);
		}

	    } else if (sdlevent.type == SDL_EVENT_MOUSE_BUTTON_UP) {
		const SDL_MouseButtonEvent &mouseEvt = sdlevent.button;
		if (mouseEvt.button == SDL_BUTTON_LEFT) {
		    mouseKeyboard.leftDown = false;
		}
		if (mouseEvt.button == SDL_BUTTON_RIGHT) {
		    mouseKeyboard.rightDown = false;
		}
		if (mouseEvt.button == SDL_BUTTON_MIDDLE) {
		    mouseKeyboard.middleDown = false;
		} else if (sdlevent.type == SDL_EVENT_MOUSE_WHEEL) {
		    eularMove.deltaScale = sdlevent.wheel.y;

		} else if (sdlevent.type == SDL_EVENT_MOUSE_MOTION) {
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

		} else if (sdlevent.type == SDL_EVENT_KEY_DOWN) {
		    const SDL_KeyboardEvent &keyboardEvt = sdlevent.key;
		    mouseKeyboard.keyMap[keyboardEvt.key] = true;

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
		} else if (sdlevent.type == SDL_EVENT_KEY_UP) {
		    const SDL_KeyboardEvent &keyboardEvt = sdlevent.key;
		    mouseKeyboard.keyMap[keyboardEvt.key] = false;
		}
	    }
	}
    }
};


void TrackBallMouseKeyboardSystem::update() {
    auto view = singleReg->view<MouseKeyboardInput, const CameraBasicAttributes, CameraEulerMoveParams>();
    auto entity = *view.begin();

	SDL_Event sdlevent;
	SDL_PollEvent(&sdlevent);
	auto &mouseKeyboard = view.get<MouseKeyboardInput>(entity);
	const auto &camMove = view.get<CameraBasicAttributes>(entity);
	auto &eularAngle = view.get<CameraEulerMoveParams>(entity);
	//首先清空欧拉角参数，不然旋转缩放会持续累加
	eularAngle.deltaScale = 0.0f;
	eularAngle.pitchAngle = 0.0f;
	eularAngle.yawAngle = 0.0f;
	if (sdlevent.type == SDL_EVENT_QUIT) {
	    mouseKeyboard.shouldQuit = true;
	} else if (sdlevent.type == SDL_EVENT_WINDOW_RESIZED) {
		auto sharingData = SharingData::getInstance();
		const WindowResizeEvent windowResizeEvent{
		    sdlevent.window.data1,
		    sdlevent.window.data2,
			false};
		sharingData->setData("resizedWindow", windowResizeEvent);
	} else if (sdlevent.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
	    const SDL_MouseButtonEvent &mouseEvt = sdlevent.button;
	    mouseKeyboard.leftDown = mouseEvt.button == SDL_BUTTON_LEFT;
	    mouseKeyboard.rightDown = mouseEvt.button == SDL_BUTTON_RIGHT;
	    mouseKeyboard.middleDown = mouseEvt.button == SDL_BUTTON_MIDDLE;
	    if (mouseKeyboard.leftDown) {
		mouseKeyboard.leftdownCursor = glm::vec2(mouseEvt.x, mouseEvt.y);
	    }
	} else if (sdlevent.type == SDL_EVENT_MOUSE_BUTTON_UP) {
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
	} else if (sdlevent.type == SDL_EVENT_MOUSE_WHEEL) {
	    // 设置camera缩放参数
	    eularAngle.deltaScale = sdlevent.wheel.y * 0.5f;
	} else if (sdlevent.type == SDL_EVENT_MOUSE_MOTION) {
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

void TrackballSystem::update() {
    auto view = singleReg->view<const CameraEulerMoveParams, CameraPose>();
    auto entity = *view.begin();
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
