export module spike_api;

// 窄 API 模块（spike）：POD 类型 + 函数 + 回调原语（mirror for_each(mask, script_cb)）。
// 接口 + 实现同文件（spike 不做声明/定义分离——分离本身不是待验假设，符号导出才是）。
export struct Vec2 {
	float x = 0.0f, y = 0.0f;
};

export using Visitor = void (*)(int index, Vec2* out);

export int api_add(int a, int b);
export Vec2 api_scale(Vec2 v, float s);
export int api_for_each(int n, Visitor fn);

int api_add(int a, int b)
{
	return a + b;
}

Vec2 api_scale(Vec2 v, float s)
{
	return {v.x * s, v.y * s};
}

int api_for_each(int n, Visitor fn)
{
	int sum = 0;
	for (int i = 0; i < n; ++i) {
		Vec2 v;
		fn(i, &v);
		sum += (int)v.x;
	}
	return sum;
}
