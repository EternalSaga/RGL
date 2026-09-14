import spike_api;

#include <dlfcn.h>
#include <cstdio>

// 宿主（spike）：dlopen 脚本 so，dlsym 约定入口，调用。
// 验证点 = 脚本对 spike_api 符号（api_add/api_for_each/api_scale）的引用
// 能否在 dlopen 时解析到宿主进程内的模块定义（宿主 exe 须 --export-dynamic）。
int main()
{
	if (api_add(1, 2) != 3) {
		std::printf("static self-test failed\n");
		return 1;
	}
	void* h = dlopen("./script.so", RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		std::printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	auto run = reinterpret_cast<int (*)(void)>(dlsym(h, "spike_script_run"));
	if (!run) {
		std::printf("FAIL dlsym: %s\n", dlerror());
		return 1;
	}
	const int r = run();
	std::printf("script result = %d (expect 133)\n", r);
	std::printf("%s\n", r == 133 ? "PASS" : "FAIL");
	return r == 133 ? 0 : 1;
}
