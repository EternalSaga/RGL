#pragma once

#include "CameraECS.hpp"
#include "ControllerECS.hpp"
namespace RGL {

class Camera {

   protected:
    entt::sigh<void(glm::mat4&, glm::mat4&, glm::vec3&)> projMatsSigh;
   public:
    virtual ShaderUniformCamProperties update() = 0;
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

    ShaderUniformCamProperties update();

    ~PerspectiveTrackballCamera();
};

}  // namespace RGL
