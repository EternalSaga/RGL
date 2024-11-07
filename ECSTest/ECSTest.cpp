#include <entt/entt.hpp>
#include <print>
#
struct AComponent {
    int a = 20;
};

struct BComponent {
    int a = 10;
};

struct CComponent {
    int a = 5;
};

int main() {
    entt::registry reg;

    auto ent1 = reg.create();

    auto ent2 = reg.create();

    reg.emplace<AComponent>(ent1);
    reg.emplace<BComponent>(ent1);

    reg.emplace<BComponent>(ent2, reg.get<BComponent>(ent1));
    reg.emplace<CComponent>(ent2);

    auto group = reg.group<BComponent>();
    auto view1 = reg.view<AComponent, BComponent>();

    auto view2 = reg.view<BComponent, CComponent>();

    view1.each([](auto& a, auto& b) {
	b.a = b.a + a.a;
    });

    view2.each([](auto& b, auto& c) {
	c.a = b.a + c.a;
	std::print("{}\n", c.a);
    });
    return 0;
}
