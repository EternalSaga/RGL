#pragma once
#include "EnttRegistry.hpp"
#include <glm/glm.hpp>

#include "ShaderManager.hpp"


namespace RGL {
struct CameraBasicAttributes {
    float sensitivity;
    float scaleSpeed;
    float speed;  // 移动速度系数
};

struct CameraPose {
    glm::vec3 position;	 // 当前camera位置
    glm::vec3 up;	 // camera本地坐标系的上方
    glm::vec3 right;	 // camera本地坐标系右方

	float pitch;
    float yaw;
};

struct CameraProjection {
    glm::mat4 viewMat;
    glm::mat4 projMat;
};


struct CameraEulerMoveParams {
    float yawAngle;
    float pitchAngle;
    float deltaScale;
    //{1,0,0}→，{-1,0,0}←
	//{0,1,0}↑，{0,-1,0}↓
    glm::vec3 direction;
};

class PerspectiveCamSystem : public SingleReg {
    // 视锥体参数
    float fovy;	 // 传入角度
    float aspect;
    float mNear;
    float mFar;

    glm::mat4 proj;

    void resizeViewport();

   public:
    PerspectiveCamSystem(float fovy, float aspect, float nearplan, float farplan);
    void update();
};



}
