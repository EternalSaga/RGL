#include <entt/entt.hpp>
#include <print>

void foo(int arg) {
    std::print("From delegate {}\n", arg);
}

struct MyStruct {
	int a = 100;
	int addToA(int b) {
	    std::print("Fron delegate struct {}\n", a + b);
	    return a + b;
	}
};

int main() {
    entt::delegate<void(int)> dele{};  // 委托的函数类型是void(int)

    dele.connect<&foo>();  // 使用委托包装foo

    dele(10);  // 调用委托函数

	dele.reset();

	MyStruct ms;
	entt::delegate<int(int)> deleWithReturn{};  // 委托的函数类型是void(int)
	deleWithReturn.connect<&MyStruct::addToA>(ms);

	auto ret = deleWithReturn(20);

	std::print("return value is {}", ret);

    return 0;
}
