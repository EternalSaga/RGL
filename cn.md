# RGL

本项目是一个个人学习计算机图形学的项目。本项目充分利用了现代C++特性。开发目标是一个对标并且加强了特性的[NCLGL](https://research.ncl.ac.uk/game/mastersdegree/graphicsforgames/introductiontonclgl/)库。NCLGL是一个以教学为目的由高校开发的图形学工具箱，它除了图形学以外，也开发了很多数学工具，对于第三方库的使用非常克制。本项目不会这样，会充分利用各种第三方库来促进开发。

## 包管理和构建

本项目使用vcpkg作为包管理工具，由于cmake和vcpkg的深度结合，所以使用cmake来进行项目构建也是理所应当的选择。同时本项目充分利用了C++20的module特性，所以当前通过测试的编译器只有msvc2022和clang-18。

### 依赖

使用vcpkg安装以下依赖

1. sdl2
2. glad
3. glm
4. spdlog
5. fmt
6. boost

### 构建

直接使用vs2022打开本项目目录，直接构建。在构建之前你可能需要修改CMakeSettings.json文件里的cmakeToolchain的值，以保证cmake能找到vcpkg。


## 文件结构

* api_abstractor

  创建一个兼容层来同时封装OpenGL和Vulkan
* entry

  整个项目的入口函数（main.cpp）
* front_end

  实现了一个Application类和Application类的渲染器接口，可以更换不同的渲染器来实现不同的渲染效果
* opengl_core

  对于OpenGL的各种封装。
* opengl_core-Practices

  使用OpenGL封装实现的各种练习用渲染器。实现了来自于front-end的渲染器接口
