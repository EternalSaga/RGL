#pragma once

#include "CameraECS.hpp"
#include "ControllerECS.hpp"
namespace RGL {

class Camera {
   public:
    virtual void update() = 0;
};

class PerspectiveTrackballCamera : public Camera {
    PerspectiveCamSystem cameraSystem;
    TrackBallMouseKeyboardSystem trackballMouseKeyboardControl;
    TrackballSystem trackballSystem;
    entt::registry* singleReg;
    entt::entity mouseKeyboardStatus;
    entt::entity cameraBasicAttributes;
    entt::entity cameraPose;
    entt::entity eulerAngle;
    entt::entity cameraProjection;

   public:
    PerspectiveTrackballCamera(float fovy, float aspect, float near, float far);

    void update();

    ~PerspectiveTrackballCamera();
};

}  // namespace RGL
