#include <entt/entt.hpp>
#include <iostream>

struct Player {
    // ... 其他玩家属性
};

struct MousePosition {
    int x;
    int y;
};


// 信号类型：玩家按下鼠标左键
entt::sigh<void()> on_left_click;

// 信号类型：鼠标位置更新
entt::sigh<void(int, int)> on_mouse_move;

// 玩家组件
struct PlayerComponent {
    void on_left_click() {
	// 发射子弹
	std::cout << u8"shoot" << std::endl;
    }

    void on_mouse_move(int x, int y) {
	// 更新瞄准方向
	std::cout << u8"aim to: (" << x << ", " << y << ")" << std::endl;
    }
};

int main() {

	PlayerComponent player;

    // 连接信号和玩家组件的成员函数
    entt::sink movesink(on_mouse_move);
	movesink.connect<&PlayerComponent::on_mouse_move>(player);

    entt::sink shootSink(on_left_click);
	shootSink.connect<&PlayerComponent::on_left_click>(player);

    // 模拟鼠标事件
	on_left_click.publish();
    on_mouse_move.publish(100, 200);

	movesink.disconnect<&PlayerComponent::on_mouse_move>(player);
    shootSink.disconnect<&PlayerComponent::on_left_click>(player);

	on_left_click.publish();
    on_mouse_move.publish(200, 300);
    return 0;
}
