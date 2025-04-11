# RGL

[中文](./cn.md)

This project is a personal learning repository for computer graphics utilizing modern C++ features. It corresponds to development goals aimed at enhancing [NCLGL](https://research.ncl.ac.uk/game/mastersdegree/graphicsforgames/introductiontonclgl/), focusing on the introduction and application of C++ in graphics for games context.
However, NCLGL delves deeper into various mathematical and physical simulations beyond graphics, with many sections being manually coded without the use of third-party libraries. This project will not follow this approach; instead, it will judiciously utilize third-party libraries to facilitate its development.

## Package Management And Building

This project leverages vcpkg as a package manager, making cmake the natural choice for build tools due to its seamless integration with vcpkg. `<s>`Additionally, it deeply incorporates the C++20 module features in development, hence currently only supports the Microsoft Visual C++ 2022(msvc) and clang-18 compiler (has already been tested on windows).`</s>` Unfortunately, my experience with C++20 modules in this project has been quite frustrating. I've encountered a wide range of compiler and linker errors that have been extremely difficult to diagnose and resolve. These issues have been highly compiler-specific, making it nearly impossible to find a consistent solution. For instance, MSVC produces a "local vftable" linking error related to virtual functions, whereas clang-18 fails to correctly deduce template arguments in certain scenarios. Given these challenges, I've been forced to temporarily abandon the use of modules. Nevertheless, I remain committed to exploring other C++20 features.

### Dependency

This repostory use vcpkg manifest mode to manage dependencies. If you have installed vcpkg properly, vcpkg will install dependencies automatically.

### Build

Open the repostory folder with vs2022 then press ctrl + shift + B to build it. Perhaps you need to edit the value of "cmakeToolchain" in the CMakeSettings.json file to make sure the vcpkg has been integrated into this project if you haven't add your vcpkg path to system environment variable "VCPKG_ROOT".

## Project Structure

* api_abstractor

  Goal: Create a compatible abstraction layer for both OpenGL and Vulkan.
* app

  Houses the main application logic.Provides a flexible renderer interface for implementing various graphical effects.
* render_core

  Encapsulates OpenGL functionalities and unimplemented cpu softrenderer.
* practices

  Demonstrates the use of OpenGL encapsulation through concrete implementations.
  All classes in this folder inherit from the Renderer class in the front-end.
