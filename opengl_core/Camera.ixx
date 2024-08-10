module;
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
export module Camera;

namespace RGL {
	namespace glcore {
		export class Camera {

			glm::mat4 viewMatrix;
			glm::mat4 projectionMatrix;
			glm::vec3 cameraPos;
		public:
			Camera(GLfloat FOV, GLfloat width, GLfloat height, GLfloat nearplane, GLfloat farPlane, glm::vec3 camPos) {
				cameraPos = camPos;
				glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, 0.0f);
				glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);

				viewMatrix = glm::lookAt(cameraPos, cameraFront, cameraUp);
				projectionMatrix = glm::perspective(FOV, width / height, nearplane, farPlane);
			}
			glm::mat4 getViewMatrix() const {

				return viewMatrix;
			}
			glm::mat4 getprojectionMatrix() const{

				return projectionMatrix;
			}

			glm::vec3 getCameraPosition()const {

				return cameraPos;
			}
		};

	}
}

