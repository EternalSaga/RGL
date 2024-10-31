#pragma once
#include "EnttRegistry.hpp"
#include <glm/glm.hpp>

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
};

struct CameraProjection {
    glm::mat4 viewMat;
    glm::mat4 projMat;
};
struct CameraEulerAngle {
    float yawAngle;
    float pitchAngle;
    float deltaScale;
};

class PerspectiveCamSystem {
    // 视锥体参数
    float fovy;	 // 传入角度
    float aspect;
    float near;
    float far;

    entt::registry* singleReg;
    glm::mat4 proj;

   public:
    PerspectiveCamSystem(float fovy, float aspect, float near, float far);
    void update();
};
}
