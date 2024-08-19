#pragma once
#include "rllogger.hpp"
#include <SDL2/SDL.h>

namespace RGL
{
class ControlLogic
{
  protected:
    virtual void onKeyboardDownCbk(const SDL_KeyboardEvent &) = 0;
    virtual void onKeyboardUpCbk(const SDL_KeyboardEvent &) = 0;
    virtual void onMouseDownCbk(const SDL_MouseButtonEvent &) = 0;
    virtual void onMouseUpCbk(const SDL_MouseButtonEvent &) = 0;
    virtual void onMouseMoveCbk(const SDL_MouseMotionEvent &) = 0;
    virtual void onMouseWheelCbk(const SDL_MouseWheelEvent &) = 0;
    virtual void onWindowResizeCbk(const SDL_WindowEvent &) = 0;
    virtual void onCursorMoveCbk(const SDL_MouseMotionEvent &) = 0;

  public:
    bool dealWithEvent();
    ControlLogic() = default;
    ;
    virtual ~ControlLogic() = default;
};

class TestLogControlLogic : public ControlLogic
{
    spdlog::logger *logger;

  protected:
    void onKeyboardDownCbk(const SDL_KeyboardEvent &keyboardEvt) override;
    void onKeyboardUpCbk(const SDL_KeyboardEvent &keyboardEvt) override;
    void onMouseDownCbk(const SDL_MouseButtonEvent &mouseEvt) override;
    void onMouseUpCbk(const SDL_MouseButtonEvent &mouseEvt) override;
    void onMouseMoveCbk(const SDL_MouseMotionEvent &mouseEvt) override;
    void onMouseWheelCbk(const SDL_MouseWheelEvent &wheelEvt) override;
    void onWindowResizeCbk(const SDL_WindowEvent &windowEvt) override;
    void onCursorMoveCbk(const SDL_MouseMotionEvent &cursorEvt) override;

  public:
    TestLogControlLogic();
    ~TestLogControlLogic() = default;
};

} // namespace RGL