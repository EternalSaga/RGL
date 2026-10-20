// cluster_path_gate.cpp — **簇路径生效闸门** + 引擎日志读取（实现；理由全文住
// `shadow_cascade_seam.cpp` 头注「簇路径生效闸门」与本头注释，此处只放实现）。
//
// 变异红线（哪个变异让我红；实跑记录 = 上一批 seam 的 V5 与本批 E6①）：
//   · 正向判据 `loaded` 恒真化（删掉 `已加载: prims=` 检查）⟹ 实机移走簇产物后本闸门不再抛，
//     但臂回显对账（`min(D=`）仍独立接住 csm 臂（seam 头注 M7 实测）——本闸门的不可替代处在
//     「首臂即抛 + 给出可执行修法 + 覆盖不查 `min(D=` 的臂（none / 参照臂）」。
//   · 负向 needle 从 `无簇产物（` 放宽成裸的 `无簇产物` ⟹ 每一臂都抛（健康 INFO 里也含这四个
//     字）= 一台永远红的仪器，与恒绿同罪。selftest 侧「健康日志不误伤」反向用例钉住这一格。
#include "cluster_path_gate.h"
#include "imgqa.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace RGL::imgqa {

namespace fs = std::filesystem;

std::string readEngineLog()
{
    const fs::path log = fs::path(projectRoot()) / "build/rgl/imgqa_run.log";
    std::ifstream in(log, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到引擎日志: " + log.string());
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/**
 * 正向 + 负向**两道**都要过：只查负向，「日志被截断/引擎没打到那行」会被读成「没回落」；
 * 只查正向，`main_scene.cppm:91` 那条「产物加载失败 → 回落现行路径」的 WARNING 会被漏掉。
 */
void requireClusterPathLive(const std::string& instrument, const std::string& armName,
    const std::string& logText)
{
    static const char* kFallbackNeedles[] = {
        "无簇产物（",   // main_scene.cppm:85 的 INFO（⚠ 裸的「无簇产物」在健康 INFO 里也有）
        "走现行路径",   // 同一条 INFO 的收尾
        "回落现行路径", // main_scene.cppm:91 的 WARNING（产物在但读失败/对账失败）
    };
    std::string hit;
    for (const char* n : kFallbackNeedles)
        if (logText.find(n) != std::string::npos)
            hit += (hit.empty() ? "" : " / ") + std::string("「") + n + "」";
    const bool loaded = logText.find("已加载: prims=") != std::string::npos;
    if (hit.empty() && loaded)
        return;
    throw std::runtime_error(
        instrument + " 臂 " + armName + "：**簇路径没生效，全部读数作废**（"
        + (hit.empty() ? "日志里没有 `已加载: prims=` 的正向证据" : "日志命中回落证据 " + hit)
        + "）。\n"
          "  这台仪器的全部读数都建立在「c1..c3 真的在画簇 LOD」上；产物缺席时引擎只打一条\n"
          "  INFO 就静默走旧路径（rgl/main_scene.cppm:85），LOD 不生效 ⟹ 各臂塌成同图 ⟹ 判据\n"
          "  会**恒绿**（对存活率仪器 = 全臂逐位相同 ⟹ 四项读数全 1.000 = 满分绿）——恒绿比红\n"
          "  危险得多，所以这里抛而不是出几条绿。\n"
          "  生成产物：`meshlet_pack <glb> --out <bin>`（清单 = cmake/RglMeshlets.cmake 的\n"
          "  RGL_MESHLET_MODELS，产物落在 build/rgl/models/<模型名>.glb.meshlets.bin）。\n"
          "  日志：" + (fs::path(projectRoot()) / "build/rgl/imgqa_run.log").string());
}

}  // namespace RGL::imgqa
