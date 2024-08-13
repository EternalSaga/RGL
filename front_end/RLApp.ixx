module;

#include <glad/glad.h>
#include <GLFW/glfw3.h>
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
import GLFramework;
import GLCheckError;

import ThreeDEffectPractice;


namespace RGL {

	using resize_cbk_type = void(int, int);
	using keyboard_cbk_type = void(int, int, int);
	using mouse_cbk_type = resize_cbk_type;
	using cursor_cbk_type = void(double, double);
	export class RLApp {


		GLFWwindow* window_;
		RendererContext* glCxt;
		static std::unique_ptr<RLApp> instance;
		static std::atomic_bool isInit;
		static std::mutex mu;
		RLApp(int width, int height, std::string title, API_TYPE api_type) {
			glfwInit();

			switch (api_type)
			{
			case RGL::API_TYPE::OPENGL46:
				glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
				glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
				break;
			case RGL::API_TYPE::CPU:
				glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
				break;
			case RGL::API_TYPE::VULKAN13:
				break;
			case RGL::API_TYPE::DX12:
				break;
			default:
				break;
			}
			window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
			if (window_ == nullptr) {
				throw std::runtime_error("failed to create window");
			}

			if (api_type == API_TYPE::OPENGL46)
			{
				this->glCxt = new glcore::GLContext(window_, width, height);
				auto interLeavedBuffer = std::make_unique<practice::BasicTransform>();
				this->glCxt->setRenderer(std::move(interLeavedBuffer));
			}
			else if(api_type == API_TYPE::CPU)
			{

			}

			
			
		}


		static std::function<resize_cbk_type> resizeCallback;
		static std::function<keyboard_cbk_type> keyboardCallback;
		static std::function<mouse_cbk_type> mouseCallback;
		static std::function<cursor_cbk_type> cursorCallback;
	
	private:
		//窗口相应回调
		static void resize_cbk(GLFWwindow* window, int width, int height) {
			resizeCallback(width, height);
		}
		//键盘回调
		static void keyboard_cbk(GLFWwindow* window, int key, int scancode, int action, int mods) {
			keyboardCallback(key, action, mods);
		}

		static void mouse_cbk(GLFWwindow* window, int button, int action, int mods) {
			mouseCallback(button, action);
		}

		static void cursor_cbk(GLFWwindow* window, double x, double y) {
			cursorCallback(x,y);
		}

	public:

		static std::unique_ptr<RLApp> getInstance(int width, int height, std::string title, API_TYPE api) {

			if (!isInit)
			{
				std::scoped_lock lo(mu);
				instance.reset(new RLApp(width, height, title, api));
				//instance= std::make_unique<RLApp>(width, height, title, api);
			}

			//static RLApp app{ width, height, title,api };
			return std::move(instance);
		}
		void run() {
			//设置各种回调
			glfwSetWindowSizeCallback(window_, resize_cbk);
			glfwSetKeyCallback(window_, keyboard_cbk);
			glfwSetMouseButtonCallback(window_, mouse_cbk);
			glfwSetCursorPosCallback(window_, cursor_cbk);
			while (!glfwWindowShouldClose(window_)) {

				//获取输入事件
				glfwPollEvents();

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
			glfwDestroyWindow(window_);
			glfwTerminate();
		}
	};

	std::function<resize_cbk_type> RLApp::resizeCallback;
	std::function<keyboard_cbk_type> RLApp::keyboardCallback;
	std::function<mouse_cbk_type> RLApp::mouseCallback;
	std::function<cursor_cbk_type> RLApp::cursorCallback;


	std::atomic_bool RLApp::isInit;
	std::unique_ptr<RLApp> RLApp::instance;
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

		auto app = RGL::RLApp::getInstance(1200, 900,"opengl_study", API_TYPE::OPENGL46);
		app->setResizeCallback(onResize);
		app->setKeyboardCallback(onSetKeyboard);
		app->setMouseCallBack(onMouseButton);
		app->setCursorCallBack(onCursorMove);
		app->run();
	}

}
