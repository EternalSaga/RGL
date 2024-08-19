#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <boost/variant2/variant.hpp>
#include <map>
using namespace boost::variant2;

namespace RGL {
	class CameraInner {

		glm::vec3 position;//当前camera位置
		glm::vec3 up;//camera本地坐标系的上方
		glm::vec3 right;//camera本地坐标系右方

	public:
		CameraInner() :position(0.0f, 0.0f, 1.0f),
			up(0.0f, 1.0f, 0.0f),
			right(1.0f, 0.0f, 0.0f)
		{

		}
		~CameraInner() = default;
		glm::mat4 getViewMatrix() const {

			const glm::vec3 front = glm::cross(up, right);//右手法则，up×right，得出camera前方向

			const glm::vec3 center = position + front;//从摄像机位置往前看，就是center

			return glm::lookAt(position, center, up);
		}

	};

	class OrthographicCamera {
		//组合方式使用cam父类
		CameraInner cam;

		//正交box？参数
		float left;
		float right;
		float top;
		float bottom;
		float near;
		float far;

	public:
		OrthographicCamera(float l, float r, float t, float b, float n, float f) :
			left(l), right(r), top(t), bottom(b), near(n), far(f), cam()
		{

		}
		~OrthographicCamera() = default;
		glm::mat4 getProjectionMatrix() const {
			return glm::ortho(left, right, bottom, top, near, far);
		}
	};


	class PerspectiveCamera {
		//组合方式使用cam父类
		CameraInner cam;
		//视锥体参数
		float fovy;//传入角度
		float aspect;
		float near;
		float far;
	public:
		PerspectiveCamera(float fovy, float aspect, float near, float far) :
			fovy(fovy), aspect(aspect), near(near), far(far), cam()
		{}
		glm::mat4 getProjectionMatrix() const {
			return glm::perspective(glm::radians(fovy), aspect, near, far);
		}
	};

	using Camera = variant<OrthographicCamera, PerspectiveCamera>;

	class CamControlInner {
		bool leftDown;
		bool rightDown;
		bool middleDown;
		glm::vec2 previousCursor;
		glm::vec2 currentCursor;
		std::map<int, bool> keyMap;
		float sensitivity;
	public:
		CamControlInner() :leftDown(false), rightDown(false), middleDown(false),
			previousCursor(0.0f, 0.0f), currentCursor(0.0f, 0.0f), sensitivity(0.1f) {}
		~CamControlInner() = default;
		//void onMouse(button,action)
		//void onCursor(x,y)
		//void onKey(key,action)

		//update()
	};

	class CamControlGame
	{
	public:
		CamControlGame() {

		}
		~CamControlGame() = default;

	private:

	};

	class CamControlTrackball {
	public:
		CamControlTrackball() {

		}
		~CamControlTrackball() = default;
	};
}
