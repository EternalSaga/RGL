#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
namespace RGL {
namespace glcore {

	struct PoseComponent {
		glm::vec3 position;
	    float angleX;
	    float angleY;
	    float angleZ;
	    glm::vec3 scale;
	};

	class PhysicsSystem {
		
	public:

		PhysicsSystem() {

		}

		void update(entt::registry& registry) {
			auto view = registry.view<PoseComponent>();
			for (auto entity : view) {
				auto& pose = view.get<PoseComponent>(entity);
				//to do
			}
		}

	};


}
}  // namespace RGL
