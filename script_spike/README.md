# script_spike — 模块 × dlopen 兼容性最小验证（P0，2026-09-14）

独立子项目（同 min_mesh_crash/ 模式）：验证"C++20/23 模块编进宿主 exe、脚本 TU 经 BMI
import、dlopen 后跨边界符号解析到宿主"在 **Linux + clang 22.1.8** 是否成立。
背景与长期规划 → `docs/脚本长期规划.md`。

## 待验三点 → 结论

| # | 待验点 | 结论 |
|---|---|---|
| ① | 模块 GMF 能否产 BMI 供跨单元 `import`（`-fmodules -x c++-module -c` → .pcm + .o） | ✅ |
| ② | 宿主 **exe** 能否导出模块符号（exe 默认符号不进 .dynsym） | ✅ 须 `-Wl,--export-dynamic`；模块符号 = 普通 C++ mangle（模块名编进 mangling，`_ZW9spike_api7api_addii`） |
| ③ | 脚本 so 编译期 `import`（`-fmodule-file=spike_api=spike_api.pcm`）、运行时 dlopen 解析到宿主 | ✅ RTLD_NOW 即解析，回调（引擎→脚本函数指针）+ POD struct 过界均正常，round-trip = 133 |

**运行**：`./build.sh`（无依赖，clang++ 即可；CXX=... 可换编译器对照）。

## 机制要点（写进规划文档的依据）

- 依赖方向：脚本编译期认识 API 模块（BMI），运行时符号解析到宿主进程内同一份定义；
  引擎对脚本只认 dlsym 入口符号名，编译期零耦合。
- 脚本入口用 `extern "C"`（免 mangle 不确定性）；引擎侧 API 函数模块 mangle 不影响
  （脚本经 BMI 编译，mangle 自动一致）。
- BMI 与消费方必须同 `-std`（本 spike 全 `-std=c++23`）。

## 未验（P1 清单）

- C++ 异常跨 dlopen 边界行为（预期 terminate——红线"边界 noexcept"需实验留证）
- 脚本自身作为模块（.cppm → so）而非普通 TU
- 热重载语义：unload/reload .so 的符号回收与二次解析
- ABI 破坏的失败形态（预期 = 脚本编译期报错，这是该路线相对 C ABI 的核心优势）
- Windows（clang/LLVM）——第二梯队，暂不验
