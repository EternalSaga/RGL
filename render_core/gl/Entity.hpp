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
#include "UBO.hpp"
#include "EnttRelationship.hpp"
#include "rllogger.hpp"
namespace RGL {
namespace glcore {


struct VertArrayComponent {
    std::unique_ptr<VAO> vao;
    size_t vertCount;
    size_t idxOffset;
};


struct RenderTag {
};


class GeneralEntity : public SingleReg {
    entt::entity entity;

   public:
    template <typename T, typename... Args>
    void attachComponent(Args&&... args) {
	singleReg->emplace<T>(entity, std::forward<Args>(args)...);
    }
    inline operator entt::entity() const {
	return entity;
    }
    inline operator entt::entity() {
	return entity;
    }
    GeneralEntity() : entity(singleReg->create()) {
	
	}
    ~GeneralEntity() = default;
};



class CommonRenderEntity : public SingleReg {

    entt::entity entity;
    static void modelSystemUBO();

	static void modelSystemSimple();

    static void renderVertexArray();


   protected:
   public:
    CommonRenderEntity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale);
    ~CommonRenderEntity() = default;
    void setMesh(std::unique_ptr<Mesh> mesh, ShaderRef shader);

    static void update();
	template<typename T,typename... Args>
    void attachComponent(Args&&... args) {
	singleReg->emplace<T>(entity, std::forward<Args>(args)...);
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
