import spike_api;

// 脚本侧（spike）：普通 TU import API 模块（用户设想的"import 声明就行"形态）。
// 导出约定入口符号；回调体内调引擎函数 = 跨 dlopen 边界符号解析的实测点。
extern "C" int spike_script_run()
{
	Visitor visit = [](int i, Vec2* v) {
		v->x = (float)api_add(i, 10);
		v->y = 0.0f;
	};
	int sum = api_for_each(3, visit);  // 10+11+12 = 33（引擎驱动回调，回调代码在脚本 so 内）
	Vec2 v = api_scale({2.0f, 3.0f}, 0.5f);  // (1, 1.5) POD 过界
	sum += (int)(v.x * 100.0f);  // +100
	return sum;  // 期望 133
}
