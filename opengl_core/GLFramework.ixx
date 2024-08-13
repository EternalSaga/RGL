module;
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <stdexcept>
#include "Helpers.hpp"
#include <memory>
#include <cassert>

export module GLFramework;

import apiAbstractor;
import GLCheckError;


import GLObjWrapper;

namespace RGL {
	namespace glcore {



		export class GLContext : public RendererContext
		{
			std::unique_ptr<Renderer> renderer;
		public:
			GLContext(GLFWwindow* windowHandler, GLint viewPortWidth, GLint viewPortHeight):renderer(nullptr){
				api_type = API_TYPE::OPENGL46;
				this->window_ = windowHandler;
				glfwMakeContextCurrent(window_);
				if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
					throw std::runtime_error("Failed to initialize GLAD");
				}
				glCall(glViewport,0, 0, viewPortWidth, viewPortHeight);
				glCall(glClearColor,0.2f, 0.3f, 0.3f, 1.0f);
			}
			void setRenderer(std::unique_ptr<Renderer> renderer) override{
				this->renderer = std::move(renderer);
			}
			~GLContext()=default;

			void render() override {
				
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
