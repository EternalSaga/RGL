module;
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <stdexcept>
#include "Helpers.hpp"
#include <memory>
#include <cassert>
export module GLFramework;
import GLCheckError;


import GLObjWrapper;

namespace RGL {
	namespace glcore {


		export class GLRenderer {
		public:
			GLRenderer() = default;
			virtual void operator()() = 0;

			~GLRenderer() = default;
		};

		export class GLContext
		{
			std::unique_ptr<GLRenderer> renderer;
		public:
			GLContext(GLFWwindow* windowHandler, GLint viewPortWidth, GLint viewPortHeight):renderer(nullptr){

				this->window_ = windowHandler;
				glfwMakeContextCurrent(window_);
				if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
					throw std::runtime_error("Failed to initialize GLAD");
				}
				glViewport(0, 0, viewPortWidth, viewPortHeight);
				glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
			}
			void setRenderer(std::unique_ptr<GLRenderer> renderer) {
				this->renderer = std::move(renderer);
			}
			~GLContext()
			{
			}

			void render() {
				
				assert(renderer.get());

				
				glCall(glClear, GL_COLOR_BUFFER_BIT);

				(*renderer)();

				glfwSwapBuffers(window_);


			}
		private:
			GLFWwindow* window_;
		};
		

	}
}