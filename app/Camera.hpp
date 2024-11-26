#pragma once

#include "CameraECS.hpp"
#include "ControllerECS.hpp"
namespace RGL {

class Camera {

   protected:

   public:
    virtual void update() = 0;
    Camera() = default;
	
};

class PerspectiveTrackballCamera : public Camera {
    PerspectiveCamSystem cameraSystem;
    TrackBallMouseKeyboardSystem trackballMouseKeyboardControl;
    TrackballSystem trackballSystem;
    entt::registry* singleReg;
    entt::entity perspcativeCameraEntity;
   public:
    PerspectiveTrackballCamera(float fovy, float aspect, float near, float far);

    void update();

    ~PerspectiveTrackballCamera();
};

}  // namespace RGL
