#include <entt/entt.hpp>
#include <print>
struct EntityPosition {
	float x;
	float y;
};

struct LightPosition {
	float x;
    float y;
};

void updatePostion(EntityPosition& pos) {
	pos.x += 1;
    pos.y += 1;
}

int main() {
    entt::registry reg;

    auto normalEntity = reg.create();
    auto lightEntity = reg.create();

	reg.emplace<EntityPosition>(normalEntity, 0.0f, 0.0f);

	reg.emplace<LightPosition>(lightEntity, 0.0f, 0.0f);

	reg.emplace<entt::sigh<void(EntityPosition & pos)>>(normalEntity);



   
    return 0;
}
