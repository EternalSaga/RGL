
#include "RLApp.hpp"
#include <string>
#include "api_types.hpp"
#include <memory>
#include <stdexcept>
#include <mutex>
#include <functional>
#include "Helpers.hpp"
#include <iostream>
#include "rllogger.hpp"
#include "GLFramework.hpp"
#include "ThreeDEffect.hpp"
#include "SRFramework.hpp"
#include "SoftwareRender.hpp"
namespace RGL {

	

	std::function<resize_cbk_type> RLApp::resizeCallback;
	std::function<keyboard_cbk_type> RLApp::keyboardCallback;
	std::function<mouse_cbk_type> RLApp::mouseCallback;
	std::function<cursor_cbk_type> RLApp::cursorCallback;


	std::atomic_bool RLApp::isInit;
	std::shared_ptr<RLApp> RLApp::instance;
	std::mutex RLApp::mu;
	void onResize( int width, int height) {
		glcore::glCall(glViewport, 0, 0, width, height);

	}
	void onSetKeyboard( int key, int action, int mods) {
		auto logger = RGL::RLLogger::getInstance();
		logger->info("press key{}", key);
	}

	void onMouseButton(int button,int action) {
		auto logger = RGL::RLLogger::getInstance();
		logger->info("press button{},action {}", button,action);
	}

	void onCursorMove(double x, double y) {
		auto logger = RGL::RLLogger::getInstance();
		logger->info("the cursor posision is ({},{})", x, y);
	}


	bool RLApp::dealWithEvent() {
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

	RLApp::RLApp(std::shared_ptr<SDLWindow> window, API_TYPE api_type) :sdlWindow(window) {



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

	void RLApp::run() {
		bool shoudQuit = false;

		while (!shoudQuit) {

			shoudQuit = dealWithEvent();

			glCxt->render();

		}
	}

	void app() {
		auto window = std::make_shared<SDLWindow>(1200, 900, "opengl_study", API_TYPE::CPU);
		RLApp app(window, API_TYPE::CPU);
		app.setResizeCallback(onResize);
		app.setKeyboardCallback(onSetKeyboard);
		app.setMouseCallBack(onMouseButton);
		app.setCursorCallBack(onCursorMove);
		app.run();
	}

}
