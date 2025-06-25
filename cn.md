# RGL

本项目是一个个人学习计算机图形学的项目。本项目充分利用了现代C++特性。开发目标是一个对标并且加强了特性的[NCLGL](https://research.ncl.ac.uk/game/mastersdegree/graphicsforgames/introductiontonclgl/)库。NCLGL是一个以教学为目的由高校开发的图形学工具箱，它除了图形学以外，也开发了很多数学工具，对于第三方库的使用非常克制。本项目不会这样，会充分利用各种第三方库来促进开发。

## 包管理和构建

本项目使用vcpkg作为包管理工具，由于cmake和vcpkg的深度结合，所以使用cmake来进行项目构建也是理所应当的选择。<s>同时本项目充分利用了C++20的module特性，所以当前通过测试的编译器只有msvc2022和clang-18。</s>不幸的是，我在本项目中实践使用C++20modules特性的过程中，遇到了各种难以理解的编译器错误乃至各种链接器错误。它们有些可以通过使用clang-18规避，有些又需要使用msvc规避。我很难将它们完全在同一个编译器中完全规避。比如说，一个有关于"local vftable"的虚函数链接错误会在msvc里发生，而在clang-18里，它会对某些模板函数的模板参数难以进行正确推断。所以不得已下，我暂时放弃了使用modules，不过，我还是会尽可能的使用更多其他C++20新特性。

### 依赖

本项目使用vcpkg的清单模式，如果你正确安装了vcpkg，那么如果你使用visual studio打开本项目的时候就会自动安装所需依赖

### 构建

直接使用vs2022打开本项目目录，直接构建。如果你没有把vcpkg的路径添加到环境变量“VCPKG_ROOT”，在构建之前你可能需要修改CMakeSettings.json文件里的cmakeToolchain的值，以保证cmake能找到vcpkg。


## 文件结构

* api_abstractor

  创建一个兼容层来同时封装OpenGL和Vulkan
* app
  整个项目的入口函数和顶层控制逻辑
* front_end

  实现了一个Application类和Application类的渲染器接口，可以更换不同的渲染器来实现不同的渲染效果
* opengl_core

  对于OpenGL的各种封装。
* opengl_core-Practices

  使用OpenGL封装实现的各种练习用渲染器。实现了来自于front-end的渲染器接口
