
#include "RLApp.hpp"
#include "Camera.hpp"

#include "GLFramework.hpp"
#include "Helpers.hpp"
#include "SRFramework.hpp"
#include "SoftwareRender.hpp"
#include "LightEffect.hpp"
#include "api_types.hpp"
#include "rllogger.hpp"


#include "ControllerECS.hpp"
#include <memory>
#include <mutex>

namespace RGL {

std::atomic_bool RLApp::isInit;
std::shared_ptr<RLApp> RLApp::instance;
std::mutex RLApp::mu;
void onResize(int width, int height) {
    glcore::glCall(glViewport, 0, 0, width, height);
}
void onSetKeyboard(int key, int action, int mods) {
    auto logger = RGL::RLLogger::getInstance();
    logger->info("press key{}", key);
}

void onMouseButton(int button, int action) {
    auto logger = RGL::RLLogger::getInstance();
    logger->info("press button{},action {}", button, action);
}

void onCursorMove(double x, double y) {
    auto logger = RGL::RLLogger::getInstance();
    logger->info("the cursor posision is ({},{})", x, y);
}

RLApp::RLApp(std::shared_ptr<SDLWindow> window)
    : sdlWindow(window) {
}

void RLApp::run() {
    bool shoudQuit = false;

    while (!shoudQuit) {
	renderCtx->render();
	shoudQuit = ShouldQuit();
    }
}

void RLApp::setRendererContext(std::unique_ptr<RendererContext> ctx) {
    renderCtx = std::move(ctx);
}

std::unique_ptr<RendererContext> CreateContext(API_TYPE api_type, std::shared_ptr<SDLWindow> sdlWindow, std::shared_ptr<Camera> camera) {
    std::unique_ptr<RendererContext> renderCxt;

    if (api_type == API_TYPE::OPENGL46) {
	renderCxt = std::make_unique<glcore::GLContext>(*sdlWindow, sdlWindow->getWidth(),
	    sdlWindow->getHeight());
	auto interLeavedBuffer = std::make_unique<practice::PhongSPMaskExec>(camera);
	renderCxt->setRenderer(std::move(interLeavedBuffer));
    } else if (api_type == API_TYPE::CPU) {
	renderCxt = std::make_unique<swr::SoftwareRenderContext>(sdlWindow);
	SDL_Surface *surface = SDL_GetWindowSurface(*sdlWindow);
	auto cpurenderer = std::make_unique<swr::TestCPURender>(surface);
	renderCxt->setRenderer(std::move(cpurenderer));
    }
    return renderCxt;
}

void app() {
    constexpr auto api = API_TYPE::OPENGL46;

    auto window =
	std::make_shared<SDLWindow>(720, 480, "opengl_study", api);
    RLApp app(window);

    std::shared_ptr<Camera> camera = std::make_shared<PerspectiveTrackballCamera>(60.0f, static_cast<float>(window->getWidth()) / window->getHeight(), 0.1f, 1000.0f);

    auto renderCtx = CreateContext(api, window, camera);

    app.setRendererContext(std::move(renderCtx));

    app.run();
}

}  // namespace RGL
