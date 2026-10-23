# 债-140 §22/§27 独立最小复现器（任务 8 + 任务 13）

> **仓库端口（2026-10-23）**：本目录 = 复现器正典（自 `ab/repro/` 迁入，构建只依赖 PATH 上的
> `glslangValidator` + 系统 libvulkan，任何机器 `./build.sh` 即走）。两枚 segfault core 原片
> 留在 8060S 的 `ab/repro/materials/`（git 不放二进制 core，附件用 `ab/upstream/post/` 的 tar 包）。
>
> **三卡复测口径**（非 gfx1151 预期全净 = 投递前的最后自证）：
> ```bash
> ./build.sh
> ./repro --chain 1 --mode mdi --rows 1500 --dispatches 3 --frames 400 --rowvary   # 金标准格 R3
> ./repro --triv 1 --mode mdi --rows 1500 --dispatches 3 --frames 400              # 净锚
> ./repro --chain 1 --mode direct --rows 1500 --dispatches 3 --frames 400          # 净锚
> ```
> 挂判据 = 进程 120s 不动 / 内核 `ring gfx timeout` / MODE2 复位（`journalctl -k`）。
> `--heap` 变体需要 VK_EXT_descriptor_heap（NV/Intel 上没有，跳过属预期）。

> **🔴 2026-10-23 任务 13 更新：复现成立。**§22 的"独立复现失败"输在 mesh 是 trivial；
> 补上引擎形态真取数链（`--chain`）后：**R3 = 12/14 挂**（经典管线+描述符集+MDI+链 mesh，
> 无任何非常规扩展），R1 trivial+heap 0/14、R4/R4b 直派 0/14。必要条件 = **MDI × mesh 真解引用
> buffer 数据**（heap/SO 非必要）。金标准命令行：
>
> ```bash
> ./repro --chain 1 --mode mdi --rows 1500 --dispatches 3 --frames 400 --rowvary   # 12/14
> ```
>
> 崩溃签名 = 引擎同族（gfx ring timeout→MODE2）+ 驱动 CPU 侧 NULL `call *(%rax)`
> （libvulkan_radeon.so+0x15eaa3，core ×2 已存）。素材包 = `materials/`。
> 矩阵与判读 → 取证文档 §27；提交稿 A 已改为事实。以下保留 §22 原文。


单文件 Vulkan 程序（无引擎依赖）：把 §21 的剂量-响应曲线搬到引擎外。
**结论：独立复现失败——17 臂 150 轮 0 挂 0 fault**，含与引擎 M-1b 逐位同形态的
`DISPATCH_MESH_INDIRECT_MULTI` + `DRAW_INDEX_ENABLE=1` 包（dumpibs 实证）。
读数与判读 → 取证文档 §22。

## 构建与运行

```bash
./build.sh            # glslangValidator(--target-env spirv1.4) + clang++ -lvulkan
./repro --tasks 1530 --x 6 --frames 400 --dispatches 3   # 输出 JSON（校验和=工作量证明）
python3 run_scan.py <label> <轮数> <repro 参数...>       # 扫描器（journalctl 复位/fault 同族检测）
```

环境：内核 7.2.11 + 固件 20261019 + Mesa 26.2.4（与 §21 同环境）。
挂判据 = rc≠0（4=DEVICE_LOST，124=超时）；每轮跑前 `pgrep -cx rgl`=0。

## 参数（每枚 = 一维加回）

| 旗标 | 维度 | 引擎对应 |
|---|---|---|
| `--tasks T --x X` | 每派发 task 数 × 每 task Emit 组数 | 剂量 |
| `--mode mdi --rows R --stride S` | 间接行表派发（行 X=task 数） | MDI 1530 行 stride 24 |
| `--xsrc buffer` | Emit 组数 = buffer 数据驱动（§21 头号未测维） | payload word5 |
| `--reuse persist` | 持久映射每帧改写（ring×4） | ring×2 memcpy |
| `--frames F --dispatches D` | 帧循环 × 每帧段数 | 400 帧 × 3 级联 |
| `--async` | 帧间不等 fence（多帧 in-flight） | 引擎不同步 |
| `--interleave` | MDI 之间插经典 vertex 全屏 draw | 队列有别的活 |
| `--vary` | 行 gcX∈{0,1} 孔洞流 + 组数 2..18 可变（LCG 定种子） | 引擎行形态 |
| `--heavy` | mesh 三角形=全屏（每组执行时长 ~µs 级） | 引擎节奏 |
| `--meshdrawid` | mesh 入口读 gl_DrawID（M-1b 因果变量；🔴 必须配 `--notask`——task+mesh 读 DI 违反 VUID-pStages-09631；DI 与否 = 两个编译变体，死代码也算静态使用） | M-1b |
| `--meshread` | DI → 行表 load → 二次 load 双间接链 | 引擎 DI 消费形态 |
| `--rebind` | 派发间重绑管线（DI 基址 SGPR 重设） | 每级联重绑 |
| `--notask` | mesh-only 管线（行 X=mesh 组数）= **真 M-1b 形状** | preN10 |
| `--intermesh N` | MDI 之间插大型 mesh 直派 N 组（§22.1 候选②） | 主场景 27k 簇/帧 |
| `--intertask T` | 直派臂 = task 驱动数据驱动组数（两路都吃 meshlet 机械） | 同上 |
| `--twopass` | 两臂分属两 render pass 共用同一 depth 靶（attachment 切换） | shadow/main 切换 |
| `--interdim2` | 直派臂 meshlet 线程维度 128≠3（DIM 寄存器重编程，dumpibs 实证） | 主场景≠阴影维度 |
| `--chain 1` | **§27 真取数链**：引擎形态 task（行表→payload→Emit g）+ 五级链 mesh（区间表→簇条目→Tris/Remap→池，取回值进位置）；行 stride 恒 24 | shadow_meshlet task/mesh |
| `--triv 1` | 链 mesh 的 CHAIN=0 变体（payload 只参与算术）= R1 锚 | §26 S1 |
| `--heap` | SO + descriptor heap 绑定（引擎形态；不加 = 经典集+管线） | shadow_meshlet_mdi 条目 |
| `--mode direct`（配 `--chain`）| 直派 grid=行数 + task `SV_GroupID.x` 行号 + gcX 早退 = R4 | §26 S3 镜像 |
| `--rowvary` | 行表内容每帧改写（LCG 续流） | 引擎逐帧剔除变化 |

## 扫描表（全 0 挂；逐轮 jsonl 在 results/）

| 臂 | 形状 | 剂量/派发 | 轮 | 挂 |
|---|---|---|---|---|
| m2_f1_g{64x7,1530x1,1530x3,1530x6,1530x12} | CTS 形状（direct+const 单次） | 448–18360 | 50 | 0 |
| m2_f400_g9200 | +400 帧×3 段（仍 sync+fresh） | 9180 | 10 | 0 |
| m3_engineshape | mdi+buffer+persist+stride24（sync） | 9180 | 10 | 0 |
| m3_A/B/C | +async / +interleave / +vary | 9180 | 30 | 0 |
| m3_D | +剂量 18400 | 18360 | 10 | 0 |
| m3_E | +heavy（引擎节奏） | 9180 | 10 | 0 |
| m4_m1b | ~~+mesh 读 DrawIndex~~ **作废撤回**：task+mesh 读 DI 违反 VUID-pStages-09631（非法组合，见 §22.1） | 9180 | 6 | — |
| m5_m1b_chain | ~~+DI 双间接内存链~~ **作废撤回**（同上） | 9180 | 6 | — |
| m6_m1b_rebind | ~~+派发间重绑~~ **作废撤回**（同上） | 9180 | 6 | — |
| m7_true_m1b | **mesh-only 真 M-1b**（包=dumpibs 实证同形态；mesh-only 无 09631 问题） | ~14500 | 6 | 0 |
| m8_m1b_heavy | 真 M-1b + 引擎节奏 | ~14500 | 6 | 0 |
| m7r/m8r | 上两臂在修正后二进制重跑（补开 multiDrawIndirect/shaderDrawParameters，验证层 0 错误） | 14500 | 12 | 0 |
| m9/m10/m11 | §22.1 候选②：+27k 组/帧 mesh 直派交错 / 直派臂 task 数据驱动 / 两 pass 共用 depth 靶 | ~55k/帧 | 30 | 0 |
| m9b/m11b | +DIM 重编程（直派臂 128 线程 vs MDI 臂 3，dumpibs 实证并存） | ~55k/帧 | 20 | 0 |

## 判读

引擎在同样剂量下 50–90% 挂率、M-1b 形态 6/6 确定性挂——复现器把**标准 API 能表达的
全部维度**都摆齐了仍 **0/194 有效轮**（150 轮 − 撤回 18 轮 + 候选②批 50 轮 + m7r/m8r 12 轮）
⟹ 触发前提在引擎独有结构里，剩余候选按嫌疑排序：

1. **shader object + descriptor heap 绑定路径**（复现器是经典管线 + descriptor set；
   RADV 两条路在 set_sh_reg 次序 / pre-rast 状态编程上有差异——MECHANISM.md 判"同路"
   是源码层判断，未被本实验豁免）。**唯一未动的大变量，半天工量，待用户拍板。**
2. ~~繁忙命令流交互~~ **§22.1 探针范围内洗清（0/50）**：大型 mesh 直派交错 / 直派臂
   task 数据驱动 / 两 pass 共用 depth 靶 / DIM 重编程（dumpibs 实证）全净。
   探针边界：交错臂着色器仍是 trivial 三角形，"交错臂的内存压力"未覆盖。
3. 巨型 mesh 着色器的 SGPR 压力（draw_id SGPR 在大 user 区间中部 + push 混布）；
   次选探针 = 给交错臂上引擎级内存流量（簇表/场景数据读进直派臂着色器）。

对上游的含义：独立复现器**没能**成为新素材（提交稿维持引擎配方 + preN10 worktree 口径）；
但"标准 API 形状全绿 + 引擎形态挂"这个负读数本身收窄了嫌疑面——把"shader object/heap
绑定路径"从"无所谓"升格为"头号未排除变量"。
