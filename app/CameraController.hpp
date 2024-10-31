#pragma once
#include "ControlLogic.hpp"
#include <map>



namespace RGL {



struct CamControlInner {
   protected:
    bool leftDown;
    bool rightDown;
    bool middleDown;

    glm::vec2 currentCursor;
    glm::vec2 leftdownCursor;
    glm::vec2 rightdownCursor;
    glm::vec2 lastCursor;
    std::map<int, bool> keyMap;
    float sensitivity;
    float scaleSpeed;

    float pitch;  // 俯仰角
    float yaw;	  // 水平角度
    float speed;  // 移动速度系数

    virtual void doPitch(float angle) = 0;
    virtual void doYaw(float angle) = 0;

   public:
    CamControlInner();
    ~CamControlInner() = default;
    void setSensitivity(float sensitivity) { this->sensitivity = sensitivity; }
};

class CamControlGame : public ControlLogic, public CamControlInner {
   public:
    CamControlGame();
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

class CamControlTrackball : public ControlLogic, public CamControlInner {
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

// 判断当前鼠标指针是否往回移动
inline bool currentCursorBetween(const glm::vec2 &downCursor, const glm::vec2 &lastCursor, const glm::vec2 &currentCursor) {
    // 如果鼠标在左键点下时候的点和上一个更新的点的中间，那么中间点和两边点的夹角应该是钝角，点乘小于0
    return glm::dot(currentCursor - lastCursor, currentCursor - downCursor) < 0;
}
}  // namespace RGL
