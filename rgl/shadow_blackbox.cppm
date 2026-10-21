export module shadow_blackbox;

import rlstd;                  // uint64_t / span（BMI 不替我带 STL 名，接口里出现就得自己 import）
import frame_draw_list;      // FrameDrawList / MdiShadowSegment
import meshlet_shadow_args;  // MeshletShadowArgsService（heap 槽台账）

/**
 * shadow_blackbox —— 债-140 **A1 黑匣子**与债-143 **常开行表审计**的引擎侧接线（rlframe）
 *
 * ## 为什么单独成一个模块（而不是写在 `vkrenderer_passrec.cppm` 里）
 * 填一帧记录要读 `MdiShadowSegment` 的十个字段 + 算摘要 + 取槽台账，实测 43 行代码 ——
 * 塞进 `prepareMdiShadowRows` 会把它的认知复杂度从 22 顶到 33（红线 30），而
 * `vkrenderer_passrec.cppm` 本来就在 491/500 代码行上。诊断设施的接线**不该把宿主文件
 * 顶过红线**，也不该让宿主函数变成"半张黑匣子"。
 *
 * ## 为什么在 rlframe
 * 判据 = 「需不需要一个 VkDevice 才能跑」：本函数不碰设备，但它吃的 `FrameDrawList` 住
 * rlframe、`MeshletShadowArgsService` 住 rlscene ⟹ 依赖只能向下 ⟹ rlframe。
 * 纯逻辑（编码 / 摘要 / 谓词）全部在 `blackbox`（rlbase，`just test` 可钉），这里只有接线。
 */

namespace RGL {

/**
 * 记一帧 MDI 阴影段的现场（门 `diagnostics.blackbox` 关 = **首行即退**，逐位不变）。
 *
 * 调用时机 = 本帧所有段都 `upload` 成功**之后**（`prepareMdiShadowRows` 末尾）：这一帧的
 * 现场就是"这些行确实写进去了"之后的状态。upload 被拒的那条路径走 throw，根本到不了这里
 * ——所以记录里出现的每一段都是已落 ring 的段。
 *
 * 🔴 两条不许超范围的口径：
 *  · 摘要是 **CPU 侧写进去的那段字节**，不是"GPU 看到的内容"——本机统一内存，读回 mapped
 *    指针是恒真判据（★1-a0 记过）。摘要的价值是**跨帧可比**（同段摘要一位不变 = 没被重写）。
 *  · `heap_slots` 记的是「本条目**会读**的那些槽」，不是「本帧写入的槽」（行表描述符启动期
 *    写一次不动，每帧没有写槽动作）。
 *
 * @param list  本帧 draw list（`mdiSegments` 为空 = 门关/回落，直接返回）
 * @param frame 帧号（`FrameSession::frameNumber()`，与引擎其它日志同口径）
 * @param args  行表服务（可空 = 载体未接，此时槽台账记空、其余照记）
 */
export void recordShadowMdiBlackbox(const FrameDrawList& list, uint64_t frame,
	const MeshletShadowArgsService* args);

/**
 * 债-143 **CPU 断言**（常开审计，🔴 不经任何门）：逐帧检查本帧**实际建出的** MDI 阴影行，
 * 每行「`clusterStart` + 最大簇内序」必须落在该对象 `ObjectClusterRange::count` 内
 * （判据唯一真相源 = `blackbox::shadowRowRangeViolations`，与 GPU P2 同一条的第二份拼写）。
 *
 * ## 为什么装在 upload 之前的这里，而不是装配期的选级落表循环
 * 落表循环只能查**表**（`lodTable` vs `ClusterRangeTable`，静态、启动期一次）；这里查的是
 * **行**——行是每帧从候选重拼的成品（word[3]/[4]/[5] 三列），表对了行仍可能错（构建器漂移、
 * 未来任何动态表形态）。且此处帧号、行字节、scene data 区间表镜像三样同时在场，
 * WARN 的上下文能与 GPU `MESH_SELFCHECK` 的字段逐项对上（同源判定的抓手）。
 *
 * ## 观测者效应下界
 * 只读不写不拦（违例照上传——本断言是摄像头不是闸门，②的修复落地前不许改变任何行为）；
 * 每帧每行一次 64 位加法 + 一次比较（≤ 4773 行/帧，纳秒级）；通过时不逐帧出声，
 * 装桩证据 = 首次有段可审时一行 INFO（先例 = 黑匣子开录 / heap_slot_audit）。
 *
 * ## 限流
 * 违例通常是**静态的**（同一批行每帧同形）⟹ 每帧只 WARN 前 `kAuditWarnPerFrame` 条，
 * 超出并入一条总数行（总数恒出声，限流只限明细——"总量在不在"不许被限掉）。
 *
 * @param list  本帧 draw list（`mdiSegments` 空 = 门关/回落 = 无行可审 = 静默返回）
 * @param frame 帧号（`FrameSession::frameNumber()`，与黑匣子同口径）
 */
export void auditShadowMdiRows(const FrameDrawList& list, uint64_t frame);

}  // namespace RGL
