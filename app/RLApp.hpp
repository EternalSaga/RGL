#pragma once
#include "Camera.hpp"
#include "CameraController.hpp"
#include "SDLWindow.hpp"
#include "api_types.hpp"
#include <glad/glad.h>
#include <memory>
#include <mutex>
#include <utility>



namespace RGL
{

class RLApp
{

    std::unique_ptr<RendererContext> renderCtx;
    static std::shared_ptr<RLApp> instance;
    static std::atomic_bool isInit;
    static std::mutex mu;

    std::shared_ptr<SDLWindow> sdlWindow;


  std::unique_ptr<Camera> camera;
  
  private:
    std::unique_ptr<ControlLogic> controlLogic;
    

  public:
    RLApp(std::shared_ptr<SDLWindow> window);


    void run();

    void setRendererContext(std::unique_ptr<RendererContext> ctx);

    ~RLApp() = default;
};
void app();
} // namespace RGL
