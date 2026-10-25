module;
#include <sol/sol.hpp>
#include <glm/glm.hpp>
export module lua_read;

import rlstd;

/**
 * lua_read —— 配置层公共的 Lua 表读取 helper（rlbase，零设备）。
 *
 * 为什么单独成模块（相对坐标计划批 0）：`readVec3` 原本有两份逐行同构的副本
 * （`scene_config_impl.cppm` 的具名函数、`camera_config_impl.cppm` 的
 * `readCameraPlacement` 内同名 lambda）。批 2 要改它的返回类型，两份各改一遍 =
 * 亲手制造两份新漂移，故先收敛成这一份。两侧都只 import 本模块，**不互相 import**
 * （scene_config 与 camera_config 都在「零 entt」名单里，且 camera_config 不得依赖
 * 执行模块）。
 *
 * 工具侧还有一份 `tools/imgqa/engine_lua_edit.cpp` 的 `parseLuaVec3`，它**不合并**：
 * 不吃 sol2（手撕字符串）、已经是 double，且工具与引擎分属不同构建面。
 */

namespace RGL {

/**
 * Lua 三元表读取（`{x, y, z}` → `glm::dvec3`；缺字段 / 非表 / 分量不齐 → fallback）。
 *
 * 🔴 **返回 double 是刻意的**（批 2 第一刀的类型契约）：位置这条数据的存储层要升成
 * double 绝对层，而读取侧若仍先落 float，绝对层拿到的就是**已经舍过一遍的副本**
 * （AGENTS 哲学第 1 条：单一来源不承重，访问路径必须消费那个来源）。Lua 的数字本来
 * 就是 double（sol2 的 `lua_Number` = `double`），读成 double 是零成本的**原样搬运**。
 *
 * 因此**只此一份解析**，精度出口在调用方：要 float 的字段（方向量 / 颜色 / 欧拉角这些
 * 落库是 float 契约的量）在**落库处**显式 `glm::vec3(...)` 收窄，而不是再造一个
 * `readVec3` 的 float 版——两份同构解析 = 两份会各自漂的键名与形状判据。
 * 收窄点逐处注释「落库前收窄，绝对层保持 double」。
 *
 * @param root 配置（子）表
 * @param key 字段名
 * @param fallback 缺省值（调用方传**现值**，「缺键 = 不改」的语义就落在它身上）
 * @return 解析值或 fallback，精度 = double（未经 float 舍入）
 */
export glm::dvec3 readVec3(sol::table root, const char* key, glm::dvec3 fallback) {
	sol::table t = root[key];
	if (!t.valid() || !t.is<sol::table>())
		return fallback;
	sol::optional<double> x = t[1];
	sol::optional<double> y = t[2];
	sol::optional<double> z = t[3];
	if (x && y && z)
		return glm::dvec3(*x, *y, *z);
	return fallback;
}

}  // namespace RGL
