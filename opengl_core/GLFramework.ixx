module;
#include <glad/glad.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
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
			SDL_GLContext glContext;
			std::unique_ptr<Renderer> renderer;
		public:
			GLContext(SDL_Window* windowHandler, GLint viewPortWidth, GLint viewPortHeight):renderer(nullptr) {
				api_type = API_TYPE::OPENGL46;
				this->window_ = windowHandler;
				//先设置属性，再创建context
				SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
				SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
				SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
				SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

				SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
				SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
				constexpr bool gl_debug = true;
				const auto context_flags = SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG | (gl_debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
				SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, context_flags);

				SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);//双缓冲
				glContext = SDL_GL_CreateContext(window_);

				if (glContext == nullptr)
				{
					throw std::runtime_error("Failed to initialize opengl context");
				}

				//垂直同步
				SDL_GL_SetSwapInterval(1);
				SDL_GL_MakeCurrent(windowHandler, glContext);
				if (!gladLoadGLLoader(static_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
					throw std::runtime_error("Failed to initialize GLAD");
				}

				SDL_GL_SetSwapInterval(1);

				if (SDL_GL_LoadLibrary(NULL) != 0) {
					throw std::runtime_error(SDL_GetError());

				}

				glCall(glViewport,0, 0, viewPortWidth, viewPortHeight);
				glCall(glClearColor,0.2f, 0.3f, 0.3f, 1.0f);
			}
			
			~GLContext() {
				renderer.reset(nullptr);//手动销毁下renderer，确保opengl buffer在OpenGL context结束之前销毁
				SDL_GL_DeleteContext(this->glContext);
			}
			void setRenderer(std::unique_ptr<Renderer> renderer)override{
				this->renderer = std::move(renderer);
			}
			void render() override {
				
				assert(renderer.get());

				
				glCall(glClear, GL_COLOR_BUFFER_BIT);

				(*renderer)();

				SDL_GL_SetSwapInterval(1);


			}
		private:
			SDL_Window* window_;
		};
		

	}
}
