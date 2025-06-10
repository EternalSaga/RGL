
#include "GLFramework.hpp"
#include <imgui.h>

#include <imgui_impl_opengl3.h>
#include "ShaderManager.hpp"
#include "rllogger.hpp"
namespace RGL {
namespace glcore {

GLContext::GLContext(SDL_Window* windowHandler, GLint viewPortWidth,
    GLint viewPortHeight) {
    api_type = API_TYPE::OPENGL46;
    this->window_ = windowHandler;
    // 先设置属性，再创建context
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
	SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    constexpr bool gl_debug = true;
    const auto context_flags = SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG |
			       (gl_debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, context_flags);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);  // 双缓冲
    glContext = SDL_GL_CreateContext(window_);

    if (glContext == nullptr) {
	throw std::runtime_error("Failed to initialize opengl context");
    }

    // 垂直同步
    SDL_GL_SetSwapInterval(1);
    SDL_GL_MakeCurrent(windowHandler, glContext);


    
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
	throw std::runtime_error("Failed to initialize GLAD");
    }

    if (!SDL_GL_LoadLibrary(NULL)) {
	throw std::runtime_error(SDL_GetError());
    }

    GLint major, minor;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    
    RLLogger::getInstance()->info("Initialized OpenGL {}.{}", major, minor);

    glCall(glViewport, 0, 0, viewPortWidth, viewPortHeight);
    glCall(glClearColor, 0.2f, 0.3f, 0.3f, 1.0f);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls
							   // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends

    ImGui_ImplOpenGL3_Init("#version 130");


    glEnable(GL_DEPTH_TEST);  // 开启深度检测

    glEnable(GL_CULL_FACE);  // 启用背面剔除，对于封闭3D模型不需要绘制三角形背面
    glDepthFunc(GL_LESS);    // 设置深度检测方法
}
GLContext::~GLContext() {
    renderer.reset(nullptr);  // 手动销毁下renderer，确保opengl
			      // buffer在OpenGL context结束之前销毁
                  SDL_GL_DestroyContext(this->glContext);
}
void GLContext::render() {
    assert(renderer.get());
 //   ImGui_ImplOpenGL3_NewFrame();
 //   ImGui_ImplSDL2_NewFrame();
 //   ImGui::NewFrame();
 //   // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
 //   if (show_demo_window)
	//ImGui::ShowDemoWindow(&show_demo_window);

 //   // 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
 //   {
	//static float f = 0.0f;
	//static int counter = 0;

	//ImGui::Begin("Hello, world!");	// Create a window called "Hello, world!" and append into it.

	//ImGui::Text("This is some useful text.");	    // Display some text (you can use a format strings too)
	//ImGui::Checkbox("Demo Window", &show_demo_window);  // Edit bools storing our window open/close state


	//ImGui::SliderFloat("float", &f, 0.0f, 1.0f);		 // Edit 1 float using a slider from 0.0f to 1.0f
	//ImGui::ColorEdit3("clear color", (float*)&clear_color);	 // Edit 3 floats representing a color

	//if (ImGui::Button("Button"))  // Buttons return true when clicked (most widgets return true when edited/activated)
	//    counter++;
	//ImGui::SameLine();
	//ImGui::Text("counter = %d", counter);

	//ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
	//ImGui::End();
 //   }

    glCall(glClear, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);	 // 清空颜色缓冲，清空深度缓冲

    (*renderer)();

    SDL_GL_SwapWindow(this->window_);  // 交换缓冲区，double buffer
}
}  // namespace glcore
}  // namespace RGL
