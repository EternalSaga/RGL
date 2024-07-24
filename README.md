# RGL

This project is a personal learning repository for computer graphics utilizing modern C++ features. It corresponds to development goals aimed at enhancing [NCLGL](https://research.ncl.ac.uk/game/mastersdegree/graphicsforgames/introductiontonclgl/), focusing on the introduction and application of C++ in graphics for games context.
However, NCLGL delves deeper into various mathematical and physical simulations beyond graphics, with many sections being manually coded without the use of third-party libraries. This project will not follow this approach; instead, it will judiciously utilize third-party libraries to facilitate its development.

## Package Management And Building

This project leverages vcpkg as a package manager, making cmake the natural choice for build tools due to its seamless integration with vcpkg. Additionally, it deeply incorporates the C++20 module features in development, hence currently only supports the Microsoft Visual C++ (msvc) compiler.

### Depandance

Use vcpkg to install the following libraries.

1. glfw3
2. glad
3. glm
4. spdlog
5. fmt
6. boostBuild

### Build

Open the repostory folder with vs2022 then press ctrl + shift + B to build it. Perhaps you need to edit the value of "cmakeToolchain" in the CMakeSettings.json file to make sure the vcpkg has been integrated into this project.
