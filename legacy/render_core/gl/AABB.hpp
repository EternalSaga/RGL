#pragma once
#include <glm/glm.hpp>
namespace RGL {
namespace glcore {
class AABB {
    glm::vec3 min;
    glm::vec3 max;

   public:
    AABB();

    void extend(const glm::vec3& point);
    glm::vec3 getCenter() const;

    glm::vec3 getSize() const;

    float getLongestEdgeLength() const;

    AABB tranform(const glm::mat4& matrix) const;
};
}}