#include "SRFramework.hpp"


namespace RGL {
	namespace swr {

        void SoftwareRenderContext::drawpixel() {
          for (size_t i = 0; i < colorSurface.width; i++) {
            for (size_t j = 0; j < colorSurface.height; j++) {
              if (i % 2 == 0 && j % 2 == 0) {
                auto color = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
                colorSurface.Set(i, j, color);
              }
            }
          }
        }
        void SoftwareRenderContext::render() {
          auto clearColor = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
          colorSurface.Clear(clearColor);
          drawpixel();
          SwapBuffer(colorSurface);
          //(*renderer)();
        }
        void SoftwareRenderContext::SwapBuffer(SDL_Surface *surface) {
          SDL_Texture *texture =
              SDL_CreateTextureFromSurface(sdlRenderer, surface);
          // 画面
          if (!texture) {
            SDL_Log("swap buffer failed");
          } else {
            SDL_RenderTexture(sdlRenderer, texture, nullptr, nullptr);
            SDL_DestroyTexture(texture);
          }

          SDL_RenderPresent(sdlRenderer);
        }
        SoftwareRenderContext::~SoftwareRenderContext() {
          SDL_DestroyRenderer(sdlRenderer);
        }
        SoftwareRenderContext::SoftwareRenderContext(
            std::shared_ptr<SDLWindow> window)
            : sdlWindow(window),
              colorSurface(window->getWidth(), window->getHeight()) {
	    this->height = window->getHeight();
	    this->width = window->getWidth();
          sdlRenderer =
              SDL_CreateRenderer(*sdlWindow, "SDL_CreateSoftwareRenderer");

          if (!sdlRenderer) {
            throw std::runtime_error("cannot create renderer");
          }
        }
        } // namespace swr
}
