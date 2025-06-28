# RGL: A Modern C++ Rendering Framework
[中文](./cn.md)

<s>Readme written by LLM, reviewed by myself. </s>


RGL is a high-performance, modern C++ rendering framework built from the ground up. It leverages an **Entity Component System (ECS)** architecture at its core, utilizing modern C++20/23 features and a data-oriented design to create a flexible and powerful graphics engine. What started as a learning project has evolved into a robust framework capable of handling complex scenes, advanced rendering techniques, and a sophisticated asset pipeline.

## ✨ Key Features

-   **Modern Graphics API (OpenGL 4.6):** Utilizes modern OpenGL features, including **Direct State Access (DSA)**, to minimize state changes and improve performance.
-   **ECS-Driven Architecture:** Powered by the industry-leading **EnTT** library, all scene objects are managed as entities with distinct components, promoting clean, decoupled, and highly cache-friendly code.
-   **Data-Oriented Scene Graph:** Implements a logical scene graph using **EnTT relationships** instead of traditional pointer-based trees. This allows for efficient, cache-friendly traversal and transformation updates.
-   **Advanced Rendering Pipeline:** Features a sophisticated render queue system that automatically categorizes objects into opaque, transparent, and instanced queues. It correctly handles alpha blending for transparent objects by sorting them back-to-front.
-   **Robust Asset Pipeline:** Integrates **Assimp** to load complex 3D models and their materials. The framework includes a comprehensive texture cache and can process node hierarchies, merge meshes, and handle PBR material properties.
-   **High-Performance Instancing:** Supports hardware-accelerated instanced rendering, making it possible to render thousands of objects (like grass or rocks) with minimal CPU overhead.
-   **RAII-Compliant C++ Wrappers:** Provides clean, safe, and modern C++ wrappers for OpenGL objects (`VAO`, `VBO`, `Shader`, `Texture`), ensuring proper resource management.

## 📂 Project Structure

-   `app`: Houses the main application logic and provides a flexible `Renderer` interface for implementing various graphical effects and demos.
-   `render_core`: The heart of the framework. Contains all core rendering components, including the ECS setup (`EnTT`), OpenGL wrappers, shader management, asset importers (`Assimp`), and the rendering pipeline.
-   `practices`: Demonstrates the use of the framework's features through concrete demo implementations (e.g., UBO tests, instanced rendering of models).

## 🚀 Dependencies & Building

This project uses vcpkg in manifest mode to manage dependencies. If you have vcpkg installed, dependencies will be handled automatically.

### Dependencies

-   SDL3
-   EnTT
-   GLM
-   Glad
-   spdlog
-   Assimp
-   stb_image
-   And more... (managed by `vcpkg.json`)

### Build Instructions

1.  Ensure you have **Visual Studio 2022** and a recent version of **vcpkg** installed.
2.  Clone the repository.
3.  Open the repository folder with Visual Studio.
4.  If you have not set the `VCPKG_ROOT` environment variable, you may need to edit the `cmakeToolchain` path in `CMakeSettings.json` to point to your `vcpkg.cmake` toolchain file.
5.  Build the project using `Ctrl + Shift + B`.

## 🗺️ Future Roadmap

RGL is an actively evolving project. Here are the ambitious goals planned for the future:

1.  **Bindless Textures:** Transition to `glGetTextureHandleARB` to completely eliminate the cumbersome and limited `texture unit` system, allowing for a virtually unlimited number of textures in shaders.
2.  **SPIR-V Shader Pipeline:** Integrate **SPIR-V** and **SPIRV-Cross** to move away from hardcoded shader locations (`layout(location = ...)`) and uniform names. This will allow for true shader reflection and a more robust, backend-agnostic material system.
3.  **Unified Asset Management System:** Create a comprehensive asset system using **Boost.Serialization**. This system will serialize processed models, meshes, AABBs, and SPIR-V shaders into a custom binary format for blazing-fast load times.
4.  **Advanced Rendering Topics:** Dive deeper into **Physically-Based Rendering (PBR)**, implementing image-based lighting (IBL), shadows, and other advanced lighting and post-processing effects.
5.  **Vulkan Backend:** Abstract the rendering API to support a **Vulkan** backend alongside OpenGL, fulfilling the original vision of a true cross-API framework.
6.  **(Ambitious Goal) Multi-Draw Indirect (MDI):** Implement GPU-driven rendering using `glMultiDrawElementsIndirect` to offload almost all rendering logic to the GPU, achieving maximum performance for massive scenes.
