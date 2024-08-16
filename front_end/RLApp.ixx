module;

#include <glad/glad.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include <string>

#include <memory>
#include <stdexcept>
#include <mutex>
#include <functional>
#include "Helpers.hpp"
#include <iostream>
#include <boost/hana/tuple.hpp>
#include <boost/hana/for_each.hpp>
export module RLApp;


import apiAbstractor;
import SDLWindow;
import GLFramework;

import GLCheckError;

import ThreeDEffectPractice;

import CPURender;
import SoftwareRenderPractice;

namespace RGL {

	using resize_cbk_type = void(int, int);
	using keyboard_cbk_type = void(int, int, int);
	using mouse_cbk_type = resize_cbk_type;
	using cursor_cbk_type = void(double, double);

	


	export class RLApp {

		RendererContext* glCxt;
		static std::shared_ptr<RLApp> instance;
		static std::atomic_bool isInit;
		static std::mutex mu;
		
		std::shared_ptr<SDLWindow> sdlWindow;
		


		static std::function<resize_cbk_type> resizeCallback;
		static std::function<keyboard_cbk_type> keyboardCallback;
		static std::function<mouse_cbk_type> mouseCallback;
		static std::function<cursor_cbk_type> cursorCallback;
	
	private:

		bool dealWithEvent() {
			SDL_Event sdlevent;
			bool quit = false;
			while (SDL_PollEvent(&sdlevent)) {
				switch (sdlevent.type)
				{
				case SDL_QUIT:
					quit = true;
					break;
				case SDL_MOUSEBUTTONDOWN:
					break;
				case SDL_MOUSEBUTTONUP:
					break;
				default:
					break;
				}

				//resizeCallback();
				//keyboardCallback();
				//mouseCallback();
				//cursorCallback();
				
			}
			return quit;

		}

	public:

		RLApp(std::shared_ptr<SDLWindow> window, API_TYPE api_type):sdlWindow(window) {



			if (api_type == API_TYPE::OPENGL46)
			{
				this->glCxt = new glcore::GLContext(*sdlWindow, sdlWindow->getWidth(), sdlWindow->getHeight());
				auto interLeavedBuffer = std::make_unique<practice::BasicTransform>();
				this->glCxt->setRenderer(std::move(interLeavedBuffer));
			}
			else if (api_type == API_TYPE::CPU)
			{
				this->glCxt = new swr::SoftwareRenderContext(sdlWindow);
				SDL_Surface* surface = SDL_GetWindowSurface(*sdlWindow);
				auto cpurenderer = std::make_unique< swr::TestCPURender>(surface);
				this->glCxt->setRenderer(std::move(cpurenderer));
			}

		}


		void run() {
			bool shoudQuit = false;
			//获取输入事件和事件循环
			while (!shoudQuit) {

				shoudQuit = dealWithEvent();
				//渲染
				glCxt->render();

			}
		}

		void setResizeCallback(const std::function< resize_cbk_type> resizeCallBack) {
			this->resizeCallback = resizeCallBack;
		}
		void setKeyboardCallback(const std::function<keyboard_cbk_type>keyboardCallback)
		{
			this->keyboardCallback = keyboardCallback;
		}
		void setMouseCallBack(const std::function<mouse_cbk_type> mouseCallBack) {
			this->mouseCallback = mouseCallBack;
		}
		void setCursorCallBack(const std::function<cursor_cbk_type> cursorCallBack) {
			this->cursorCallback = cursorCallBack;
		}

		~RLApp() {
			//确保自己封装的OpenGL上下文对象的销毁在glfwTerminate之前
			//不然glfw强制销毁了OpenGL之后，OpenGL的清理函数会出错
			//考虑使用栈容器封装来确保资源的初始化和释放顺序，暂时先硬编码以下
			delete this->glCxt;
			

			
		}
	};

	std::function<resize_cbk_type> RLApp::resizeCallback;
	std::function<keyboard_cbk_type> RLApp::keyboardCallback;
	std::function<mouse_cbk_type> RLApp::mouseCallback;
	std::function<cursor_cbk_type> RLApp::cursorCallback;


	std::atomic_bool RLApp::isInit;
	std::shared_ptr<RLApp> RLApp::instance;
	std::mutex RLApp::mu;
	void onResize( int width, int height) {
		glcore::glCall(glViewport, 0, 0, width, height);
		//auto logger = glcore::Logger::getInstance();
		//logger->info("call glviewport {} {}", width, height);
	}
	void onSetKeyboard( int key, int action, int mods) {
		auto logger = glcore::Logger::getInstance();
		logger->info("press key{}", key);
	}

	void onMouseButton(int button,int action) {
		auto logger = glcore::Logger::getInstance();
		logger->info("press button{},action {}", button,action);
	}

	void onCursorMove(double x, double y) {
		auto logger = glcore::Logger::getInstance();
		logger->info("the cursor posision is ({},{})", x, y);
	}


	export void app() {
		auto window = std::make_shared<SDLWindow>(1200, 900, "opengl_study", API_TYPE::CPU);
		RLApp app(window, API_TYPE::CPU);
		app.setResizeCallback(onResize);
		app.setKeyboardCallback(onSetKeyboard);
		app.setMouseCallBack(onMouseButton);
		app.setCursorCallBack(onCursorMove);
		app.run();
	}

}
