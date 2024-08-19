#pragma once
#include "ControlLogic.hpp"
#include "SDLWindow.hpp"
#include "api_types.hpp"
#include <glad/glad.h>
#include <mutex>

namespace RGL
{

class RLApp
{

    RendererContext *glCxt;
    static std::shared_ptr<RLApp> instance;
    static std::atomic_bool isInit;
    static std::mutex mu;

    std::shared_ptr<SDLWindow> sdlWindow;

  private:
    ControlLogic *controlLogic;
    ;

  public:
    RLApp(std::shared_ptr<SDLWindow> window, API_TYPE api_type);

    inline void setControlLogic(ControlLogic *controlLogic)
    {
	this->controlLogic = controlLogic;
    }
    void run();

    ~RLApp() { delete this->glCxt; }
};
void app();
} // namespace RGL
