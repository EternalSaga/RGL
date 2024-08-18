#pragma once
#include <glad/glad.h>
#include "SDLWindow.hpp"
#include "api_types.hpp"
#include <mutex>
#include <functional>
namespace RGL {
	using resize_cbk_type = void(int, int);
	using keyboard_cbk_type = void(int, int, int);
	using mouse_cbk_type = resize_cbk_type;
	using cursor_cbk_type = void(double, double);

	class RLApp {

		RendererContext* glCxt;
		static std::shared_ptr<RLApp> instance;
		static std::atomic_bool isInit;
		static std::mutex mu;

		std::shared_ptr<SDLWindow> sdlWindow;



		static std::function<resize_cbk_type> resizeCallback;
		static std::function<keyboard_cbk_type> keyboardCallback;
		static std::function<mouse_cbk_type> mouseCallback;
		static std::function<cursor_cbk_type> cursorCallback;

	private:

		bool dealWithEvent();

	public:

		RLApp(std::shared_ptr<SDLWindow> window, API_TYPE api_type);


		void run();

		void setResizeCallback(const std::function< resize_cbk_type> resizeCallBack) {
			this->resizeCallback = resizeCallBack;
		}
		void setKeyboardCallback(const std::function<keyboard_cbk_type>keyboardCallback)
		{
			this->keyboardCallback = keyboardCallback;
		}
		void setMouseCallBack(const std::function<mouse_cbk_type> mouseCallBack) {
			this->mouseCallback = mouseCallBack;
		}
		void setCursorCallBack(const std::function<cursor_cbk_type> cursorCallBack) {
			this->cursorCallback = cursorCallBack;
		}

		~RLApp() {

			delete this->glCxt;

		}
	};
	void app();
}
