#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>


namespace RGL
{
class Camera
{

public:

    glm::vec3 position; // 当前camera位置
    glm::vec3 up;	// camera本地坐标系的上方
    glm::vec3 right;	// camera本地坐标系右方

  public:
    Camera() : position(0.0f, 0.0f, 1.0f),
		    up(0.0f, 1.0f, 0.0f),
		    right(1.0f, 0.0f, 0.0f)
    {
    }
    virtual ~Camera() = default;
    glm::mat4 getViewMatrix() const
    {

	const glm::vec3 front = glm::cross(up, right); // 右手法则，up×right，得出camera前方向

	const glm::vec3 center = position + front; // 从摄像机位置往前看，就是center

	return glm::lookAt(position, center, up);
    }

    virtual glm::mat4 getProjectionMatrix() const = 0;
};

class OrthographicCamera:public Camera
{

    // 正交box？参数
    float left;
    float right;
    float top;
    float bottom;
    float near;
    float far;

  public:
    OrthographicCamera(float l, float r, float t, float b, float n, float f) : left(l), right(r), top(t), bottom(b), near(n), far(f)
    {
    }
    ~OrthographicCamera() = default;
    glm::mat4 getProjectionMatrix() const
    {
	return glm::ortho(left, right, bottom, top, near, far);
    }
    void scale(float deltaScale)
    {
    }
};

class PerspectiveCamera:public Camera
{

    // 视锥体参数
    float fovy; // 传入角度
    float aspect;
    float near;
    float far;

  public:
    PerspectiveCamera(float fovy, float aspect, float near, float far) : fovy(fovy), aspect(aspect), near(near), far(far)
    {
    }
    glm::mat4 getProjectionMatrix() const
    {
	return glm::perspective(glm::radians(fovy), aspect, near, far);
    }
    void scale(float deltaScale)
    {
    }
};




} // namespace RGL
