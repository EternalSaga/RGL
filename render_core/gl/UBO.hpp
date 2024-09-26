#pragma once
namespace RGL {
namespace glcore {
 
    class UBO {
    public:
        UBO(const std::string &name, const std::vector<glm::mat4> &data, unsigned int bindingPoint);

    };
}}