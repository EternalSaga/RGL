#pragma once
#include "ControlLogic.hpp"
#include <map>
namespace RGL
{
struct CamControlInner {
protected:
    bool leftDown;
    bool rightDown;
    bool middleDown;


    glm::vec2 currentCursor;
    glm::vec2 previousCursor;

    std::map<int, bool> keyMap;
    float sensitivity;
    float scaleSpeed;

    float pitch; // 俯仰角
    float yaw;	 // 水平角度
    float speed; // 旋转速度系数

    virtual void doPitch(float angle) = 0;
    virtual void doYaw(float angle) = 0;

  public:
    CamControlInner() : leftDown(false), rightDown(false), middleDown(false),
			currentCursor(0.0f, 0.0f),previousCursor(0.0f, 0.0f), sensitivity(0.2f), scaleSpeed(0.2f) {}
    ~CamControlInner() = default;
    void setSensitivity(float sensitivity) { this->sensitivity = sensitivity; }
};

class CamControlGame : public ControlLogic,public CamControlInner
{
  public:
    CamControlGame()
    {
	
    }
    ~CamControlGame() = default;
    void setSpeed(float speed) { this->speed = speed; }

  private:
    void doPitch(float angle) override;
    void doYaw(float angle) override;
    void onKeyboardDownCbk(const SDL_KeyboardEvent &keyboardEvt) override;
    void onKeyboardUpCbk(const SDL_KeyboardEvent &keyboardEvt) override;
    void onMouseDownCbk(const SDL_MouseButtonEvent &mouseEvt) override;
    void onMouseUpCbk(const SDL_MouseButtonEvent &mouseEvt) override;
    void onMouseMoveCbk(const SDL_MouseMotionEvent &mouseEvt) override;
    void onMouseWheelCbk(const SDL_MouseWheelEvent &wheelEvt) override;
    void onWindowResizeCbk(const SDL_WindowEvent &windowEvt) override;
    void onCursorMoveCbk(const SDL_MouseMotionEvent &cursorEvt) override;
};

class CamControlTrackball : public ControlLogic, public CamControlInner
{
  public:
    CamControlTrackball() = default;
    ~CamControlTrackball() = default;

  private:
    void doPitch(float angle) override;
    void doYaw(float angle) override;
    void onKeyboardDownCbk(const SDL_KeyboardEvent &keyboardEvt) override;
    void onKeyboardUpCbk(const SDL_KeyboardEvent &keyboardEvt) override;
    void onMouseDownCbk(const SDL_MouseButtonEvent &mouseEvt) override;
    void onMouseUpCbk(const SDL_MouseButtonEvent &mouseEvt) override;
    void onMouseMoveCbk(const SDL_MouseMotionEvent &mouseEvt) override;
    void onMouseWheelCbk(const SDL_MouseWheelEvent &wheelEvt) override;
    void onWindowResizeCbk(const SDL_WindowEvent &windowEvt) override;
    void onCursorMoveCbk(const SDL_MouseMotionEvent &cursorEvt) override;
};
} // namespace RGL