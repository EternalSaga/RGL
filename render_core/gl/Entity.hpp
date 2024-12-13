#pragma once
#include <cstddef>
#include <glm/ext/matrix_transform.hpp>
#include <glm/fwd.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include "GLObj.hpp"

#include "Mesh.hpp"
#include "Material.hpp"
#include "Shader.hpp"
#include "ShaderManager.hpp"
#include "EnttRegistry.hpp"
#include "Light.hpp"
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

struct VertArrayComponent {
    std::unique_ptr<VAO> vao;
    size_t vertCount;
    size_t idxOffset;
};

struct MaterialComponent {
    std::unique_ptr<Material> material;
};
struct RenderTag {
};


glm::mat4 GetModelMatrix(const PoseComponent& pose, const PositionComponent& position, const ScaleComponent& scale);

class CommonEntity : public SingleReg {

    entt::entity entity;
    static void modelSystem();
    static void materialSystem();
    static void lightSystem();
    static void renderVertexArray();
   protected:
   public:
    CommonEntity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale);
    void setMesh(std::unique_ptr<Mesh> mesh, ShaderRef shader);
    void setMaterial(std::unique_ptr<Material> material);

    void setLight(std::unique_ptr<Light> light);
    static void update();

	template<typename T,typename... Args>
	void attachComponent(Args... args) {
	singleReg->emplace<T>(entity, std::forward(args)...);
	}

	inline  operator entt::entity() const{
	    return entity;
	}
	inline operator entt::entity() {
	    return entity;
	}
};


}  // namespace glcore
}  // namespace RGL
