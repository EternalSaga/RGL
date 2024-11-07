#include <entt/entt.hpp>
#include <print>
struct AComponent {
    int a = 20;
};

struct BComponent {
    int a = 10;
    void getA(int& out) {
	out = a;
    }
};

struct CComponent {
    int a = 5;
};

entt::sigh<void(int&)> updateInt;

int main() {
    entt::registry reg;

    auto ent1 = reg.create();
    auto ent2 = reg.create();
    // view1
    reg.emplace<AComponent>(ent1);
    reg.emplace<BComponent>(ent1);

    // view2
    reg.emplace<CComponent>(ent2);

    auto group = reg.group<BComponent>();
    auto view1 = reg.view<AComponent, BComponent>();

    auto view2 = reg.view<CComponent>();

    entt::sink bsink(updateInt);

    view1.each([&bsink](auto& a, auto& b) {
	bsink.connect<&BComponent::getA>(b);
	b.a = b.a + a.a;
    });

    view2.each([](auto& c) {
	int b;
	updateInt.publish(b);
	c.a = b + c.a;
	std::print("{}\n", c.a);
    });
    return 0;
}
