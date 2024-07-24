module;

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>
#include "api_types.hpp"
#include <memory>
#include <stdexcept>
#include <mutex>
#include <functional>
#include "Helpers.hpp"
#include <iostream>
export module RLApp;

import GLFramework;
import GLCheckError;

import DynamicEffectPractice;

namespace RGL {

	using resize_cbk_type = void(int, int);
	using keyboard_cbk_type = void(int, int, int);

	export class RLApp {


		GLFWwindow* window_;
		std::unique_ptr<glcore::GLContext> glCxt;
		static std::unique_ptr<RLApp> instance;
		static std::atomic_bool isInit;
		static std::mutex mu;
		RLApp(int width, int height, std::string title, API_TYPE api_type) {
			glfwInit();

			switch (api_type)
			{
			case RGL::API_TYPE::OPENGL46:
				glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
				glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
				glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
				break;
			case RGL::API_TYPE::CPU:
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


			
			this->glCxt = std::make_unique<glcore::GLContext>(window_, width, height);
			auto interLeavedBuffer = std::make_unique<glcore::ColorfulShiningTriangle>();
			this->glCxt->setRenderer(std::move(interLeavedBuffer));
		}
		static std::function<resize_cbk_type> resizeCallback;
		static std::function<keyboard_cbk_type> keyboardCallback;

	private:
		//窗口相应回调
		static void resize_cbk(GLFWwindow* window, int width, int height) {
			resizeCallback(width, height);
		}
		//键盘回调
		static void keyboard_cbk(GLFWwindow* window, int key, int scancode, int action, int mods) {
			keyboardCallback(key, action, mods);
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
			while (!glfwWindowShouldClose(window_)) {
				glfwSetWindowSizeCallback(window_, resize_cbk);
				glfwSetKeyCallback(window_, keyboard_cbk);

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

		~RLApp() {

			glfwDestroyWindow(window_);
			glfwTerminate();
		}
	};

	std::function<resize_cbk_type> RLApp::resizeCallback;
	std::function<keyboard_cbk_type> RLApp::keyboardCallback;
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

	export void app() {

		auto app = RGL::RLApp::getInstance(800, 600, "opengl_study", API_TYPE::OPENGL46);
		app->setResizeCallback(onResize);
		app->setKeyboardCallback(onSetKeyboard);
		app->run();
	}

}
