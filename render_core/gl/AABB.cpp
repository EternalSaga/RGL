#include "AABB.hpp"

#include <glm/common.hpp>
#include <glm/fwd.hpp>

#include <limits>
namespace RGL {
namespace glcore {

AABB AABB::tranform(const glm::mat4& matrix) const {
    glm::vec3 corners[8] = {
	glm::vec3(min.x, min.y, min.z),
	glm::vec3(max.x, min.y, min.z),
	glm::vec3(min.x, max.y, min.z),
	glm::vec3(max.x, max.y, min.z),
	glm::vec3(min.x, min.y, max.z),
	glm::vec3(max.x, min.y, max.z),
	glm::vec3(min.x, max.y, max.z),
	glm::vec3(max.x, max.y, max.z)};

    AABB transformedAABB;
    for (int i = 0; i < 8; ++i) {
	glm::vec3 transformedCorner = matrix * glm::vec4(corners[i], 1.0f);
	transformedAABB.extend(transformedCorner);
    }
    return transformedAABB;
}

float AABB::getLongestEdgeLength() const {
    const glm::vec3 size = getSize();
    return glm::max(glm::max(size.x, size.y), size.z);
}

glm::vec3 AABB::getSize() const {
    return max - min;
}

glm::vec3 AABB::getCenter() const {
    return (min + max) * 0.5f;
}

void AABB::extend(const glm::vec3& point) {
    min = glm::min(min, point);
    max = glm::max(max, point);
}

AABB::AABB() : min(
		   glm::vec3(std::numeric_limits<float>::max())),
	       max(
		   glm::vec3(std::numeric_limits<float>::lowest())) {}

}  // namespace glcore
}  // namespace RGL