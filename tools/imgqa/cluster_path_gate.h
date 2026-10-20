#ifndef RGL_IMGQA_CLUSTER_PATH_GATE_H
#define RGL_IMGQA_CLUSTER_PATH_GATE_H
// cluster_path_gate.h — **簇路径生效闸门** + 引擎日志读取（两台跑交付配置的阴影仪器共用）。
//
//   为什么另立 TU：`shadow-cascade-seam` 与 `shadow-caster-survival` 都跑在交付配置上、都要求
//   「c1..c3 真的在画簇 LOD」这个前提成立，而前提失效是**静默**的（缺簇产物时引擎只打一条
//   INFO 就回落旧路径，`rgl/main_scene.cppm:85`）。闸门逻辑住两份 = 第二份真相——needle 名单
//   这种「读准确文案才写得对」的东西（裸的「无簇产物」在健康 INFO 里也有，seam 头注记过账），
//   漂一份就有一台仪器静默失效。缝按**关注点**切（同 `engine_lua_edit` 从 engine.cpp 拆出）：
//   本 TU 只认「日志文本 → 放行/抛」，不认识臂表、不认识任何一台仪器的读数。
//
//   全部论证（为什么这条闸门比任何判决线都重要、为什么查 `无簇产物（` 而不是裸串）住
//   `shadow_cascade_seam.cpp` 头注「簇路径生效闸门」一节，本文件不重述。
#include <string>

namespace RGL::imgqa {

/// 读本次引擎运行的日志（`build/rgl/imgqa_run.log`，`runEngineOnce` 的落点）。读不到即抛
/// ——日志缺席时闸门什么都查不了，放行 = 闸门看起来在、其实什么都没查。
[[nodiscard]] std::string readEngineLog();

/**
 * 🔴 簇路径生效闸门（抛 = rc 3，不是 Verdict）：正向证据（`已加载: prims=`）必须在场，
 * 负向证据（回落 INFO / WARNING）必须缺席，任一不满足 ⟹ 抛并点名生成产物的命令。
 * @param instrument 仪器名（进错误串，两台仪器共用本函数后要能分辨是谁在抛）
 * @param armName    臂名（进错误串定位到哪一臂）
 * @param logText    该臂的引擎日志全文（`readEngineLog()` 的返回）
 */
void requireClusterPathLive(const std::string& instrument, const std::string& armName,
    const std::string& logText);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_CLUSTER_PATH_GATE_H
