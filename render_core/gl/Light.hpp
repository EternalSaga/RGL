#pragma once
#include <glm/glm.hpp>
namespace RGL{


    class Light{
        public:
            Light(glm::vec3 lightPos, glm::vec3 lightColor, glm::vec3 lightDirection);
            ~Light();
            glm::vec3 getLightPos();
            glm::vec3 getLightColor();
            glm::vec3 getLightDirection();

    };

    class DirectionalLight : public Light{
        
    };

}