#pragma once
#include <cstddef>
#include <glm/ext/matrix_transform.hpp>
#include <glm/fwd.hpp>
#include <glm/glm.hpp>

#include <map>
#include <memory>
#include <string>
#include "GLObj.hpp"

#include "Mesh.hpp"
#include "Material.hpp"
#include "Shader.hpp"

#include "EnttRegistry.hpp"

namespace RGL {
namespace glcore {

struct PoseComponent {
    float angleX;
    float angleY;
    float angleZ;

};
struct PositionComponent {
    glm::vec3 position;
};
struct ScaleComponent {
    glm::vec3 scale;
};

struct MeshComponent {
    std::unique_ptr<VAO> vao;
    size_t vertCount;
    size_t idxOffset;
};

struct MaterialComponent {
    std::unique_ptr<Material> material;
    std::shared_ptr<Shader> shader;
};

class CommonEntity : public SingleReg {
    VAOCreater vaoCreator;
    entt::entity entity;
	std::shared_ptr<Shader> shader;
   protected:


   public:
    CommonEntity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale, std::shared_ptr<Shader> shader);
    void setMesh(std::unique_ptr<Mesh> mesh);
    void setMaterial(std::unique_ptr<Material> material);

};

class SceneManager : public SingleReg {
   private:
    
   public:
    SceneManager();

    void drawALL();
};

}  // namespace glcore
}  // namespace RGL
