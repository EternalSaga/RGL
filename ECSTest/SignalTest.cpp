#include <entt/entt.hpp>
#include <iostream>
#include <glm/glm.hpp>
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
	std::cout << "shoot" << std::endl;
    }

    void on_mouse_move(int x, int y) {
	// 更新瞄准方向
	std::cout << "aim to: (" << x << ", " << y << ")" << std::endl;
    }
};


void announcement(std::string& msg) {
    std::cout << "announcement" << msg << std::endl;
}


struct ClassRoom {
	void receive(const std::string& msg) {
	std::cout << "classroom" << msg << std::endl;
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


	entt::sigh<void(std::string msg)> classroomSig;
    entt::sink classSink(classroomSig);


	//一个sink可以链接多个不同的函数
	classSink.connect<&announcement>();

	ClassRoom classroom;

	classSink.connect<&ClassRoom::receive>(classroom);

	std::string m = "Meeting at 3 pm.";
	//这里会调用两次输出
	classroomSig.publish(m);


    return 0;
}
