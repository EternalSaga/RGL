#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>

namespace RGL {

class Camera {
   public:
    glm::vec3 position;	 // 当前camera位置
    glm::vec3 up;	 // camera本地坐标系的上方
    glm::vec3 right;	 // camera本地坐标系右方

   public:
    Camera();
    virtual ~Camera() = default;
    glm::mat4 getViewMatrix() const;
    virtual void scale(float deltaScale) = 0;
    virtual glm::mat4 getProjectionMatrix() const = 0;
};

class OrthographicCamera : public Camera {
    // 正交box？参数
    float left;
    float right;
    float top;
    float bottom;
    float near;
    float far;

   public:
    OrthographicCamera(float l, float r, float t, float b, float n, float f);
    ~OrthographicCamera() = default;
    glm::mat4 getProjectionMatrix() const;
    void scale(float deltaScale);
};

class PerspectiveCamera : public Camera {
    // 视锥体参数
    float fovy;	 // 传入角度
    float aspect;
    float near;
    float far;

   public:
    PerspectiveCamera(float fovy, float aspect, float near, float far);
    glm::mat4 getProjectionMatrix() const;
    void scale(float deltaScale);
};

}  // namespace RGL
