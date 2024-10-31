#pragma once

#include "CameraECS.hpp"

namespace RGL {

class Camera {
   public:
    virtual void update() = 0;
};

//class OrthographicCamera : public Camera {
//    // 正交box？参数
//    float left;
//    float right;
//    float top;
//    float bottom;
//    float near;
//    float far;
//
//   public:
//    OrthographicCamera(float l, float r, float t, float b, float n, float f);
//    ~OrthographicCamera() = default;
//    glm::mat4 getProjectionMatrix() const;
//    void scale(float deltaScale);
//};



}  // namespace RGL
