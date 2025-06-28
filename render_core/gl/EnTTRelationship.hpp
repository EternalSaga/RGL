
#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include "AABB.hpp"
#include "rllogger.hpp"
#include <glm/glm.hpp>
namespace RGL {
namespace glcore {
struct Transform {
   private:
    glm::vec3 rotation;	 // 欧拉角
    glm::vec3 scale;
    glm::vec3 position;

   public:
    glm::mat4 modelMatrix;

    RLLogger* logger;
    Transform(glm::vec3 position, glm::vec3 rotation, glm::vec3 scale);

    Transform(glm::vec3 position);

    void doLocalTransform();
    void setRotation(glm::vec3 rotation);
    void setScale(glm::vec3 scale);
    void setPosition(glm::vec3 position);
    void addPosition(glm::vec3 position);
    void addRotation(glm::vec3 rotation);
    void addScale(glm::vec3 scale);

    void formToAABB(const AABB& modelAABB,const float desiredSize);

    glm::vec3 getPosition() const;
};
struct Relationship {
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

void updateTransforms();

}  // namespace glcore
}  // namespace RGL
