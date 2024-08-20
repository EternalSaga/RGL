
#include "RLApp.hpp"
#include "GLFramework.hpp"
#include "Helpers.hpp"
#include "SRFramework.hpp"
#include "SoftwareRender.hpp"
#include "ThreeDEffect.hpp"
#include "api_types.hpp"
#include "rllogger.hpp"
#include <SDL_events.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace RGL
{

std::atomic_bool RLApp::isInit;
std::shared_ptr<RLApp> RLApp::instance;
std::mutex RLApp::mu;
void
onResize(int width, int height)
{
    glcore::glCall(glViewport, 0, 0, width, height);
}
void
onSetKeyboard(int key, int action, int mods)
{
    auto logger = RGL::RLLogger::getInstance();
    logger->info("press key{}", key);
}

void
onMouseButton(int button, int action)
{
    auto logger = RGL::RLLogger::getInstance();
    logger->info("press button{},action {}", button, action);
}

void
onCursorMove(double x, double y)
{
    auto logger = RGL::RLLogger::getInstance();
    logger->info("the cursor posision is ({},{})", x, y);
}

RLApp::RLApp(std::shared_ptr<SDLWindow> window, API_TYPE api_type)
    : sdlWindow(window)
{

    if (api_type == API_TYPE::OPENGL46) {
	this->glCxt = new glcore::GLContext(*sdlWindow, sdlWindow->getWidth(),
	    sdlWindow->getHeight());
	auto interLeavedBuffer = std::make_unique<practice::BasicTransform>();
	this->glCxt->setRenderer(std::move(interLeavedBuffer));
    } else if (api_type == API_TYPE::CPU) {
	this->glCxt = new swr::SoftwareRenderContext(sdlWindow);
	SDL_Surface *surface = SDL_GetWindowSurface(*sdlWindow);
	auto cpurenderer = std::make_unique<swr::TestCPURender>(surface);
	this->glCxt->setRenderer(std::move(cpurenderer));
    }
}

void
RLApp::run()
{
    bool shoudQuit = false;

    while (!shoudQuit) {

	shoudQuit = controlLogic->dealWithEvent();

	glCxt->render();
    }
}

void
app()
{
    auto window =
	std::make_shared<SDLWindow>(1200, 900, "opengl_study", API_TYPE::OPENGL46);
    RLApp app(window, API_TYPE::OPENGL46);
    TestLogControlLogic testLogic;
    app.setControlLogic(&testLogic);

    app.run();
}

} // namespace RGL
