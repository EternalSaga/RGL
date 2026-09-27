module;
#include <filesystem>

export module shader_hot_reload;

import rlstd;
import shader;  // ShaderEffect（候选/重建输入类型）
import hot_reload;
import shader_registry;
import vk_renderer;
import vk_indirect_commands;

namespace RGL {

/**
 * Shader 热重载编排器（L1 = D2「同签名才热换」；Phase 10 批 10.4；docs/phase10_plan.md §6）。
 *
 * 流程（全帧边界，由 ConfigReloadHook 挂载 poll）：
 *   watch 源树 rgl/shaders/ 全部 .slang（mtime；v1 全量重编——导入闭包不递归解析，
 *   29 源 × slangc ~0.2s ≈ 数秒阻塞，仅开启态付出）
 *   → fork slangc（编译期注入 RGL_SLANGC_PATH + RGL_SLANG_FLAGS（= 构建期
 *     SLANG_FLAGS_STR 同源派生串，D11）+ 源目录 + 临时输出路径）
 *   → 逐条目守卫：SPV 字节 hash 无变化 = 跳过；spvInterfaceSignature 与在役基线
 *     不一致 = **契约变化被拦**（warn + 保留旧管线继续跑，提示重启）
 *   → 同签名条目：buildReplacement（旧管线仍活）→ waitForIdle → DGC execution set /
 *     静态 shader 对刷成新句柄 → commitReplacement（旧句柄此刻销毁，零 in-flight）
 *   → pipelineIndex 不变 = 资产表 / cull 序列表 / heap 槽零改动，下一帧生效
 *
 * 编译失败（语法错）= warn + 全旧管线保留，等下一次保存（HotReloader 语义 = 变更
 * 触发一次，不重试风暴）。
 */
export class ShaderHotReloader {
   public:
	/**
	 * @param registry 管线表（基线来源 = 条目 spec 路径；重建 = build/commitReplacement）
	 * @param renderer 执行器（waitForIdle——换入前清 in-flight）
	 * @param dgc NVIDIA IES 路（可空 = 设备无能力，跳过 execution set 刷新）
	 * @param dgcStatic RADV 静态路（可空 = 门未开）
	 */
	ShaderHotReloader(ShaderRegistry& registry, Renderer& renderer,
		DgcCommands* dgc, DgcStaticCommands* dgcStatic);

	/** 析构：清理临时编译目录。 */
	~ShaderHotReloader();

	/** 禁拷贝（持有管线表引用与 DGC 指针，独占语义）。 */
	ShaderHotReloader(const ShaderHotReloader&) = delete;
	/** 禁赋值（持有管线表引用与 DGC 指针，独占语义）。 */
	ShaderHotReloader& operator=(const ShaderHotReloader&) = delete;

	/**
	 * 初始化：编译期三件套（slangc 路径/flags/源 shaders 目录）存在性检查 → 逐条目读
	 * 在役 SPV 建基线（hash + 接口签名）→ 登记源目录全部 .slang 监视。
	 * @return false = 降级关（宏未注入 / slangc 或源目录不存在，已日志；poll 恒 no-op）
	 */
	bool init();

	/** 帧边界轮询（ConfigReloadHook 挂点）：收集变更（多文件同帧合并）→ 一次全量重载。 */
	void poll();

   private:
	/** 单条目两阶段的编译源与在役基线。 */
	struct StageSource {
		std::filesystem::path slang;      // 源树 .slang（spec spv 路径 basename 换扩展，同 compile_slang 规则）
		uint64_t baselineHash = 0;        // 在役 SPV 的 FNV-1a（字节无变 = 免重建）
		std::string baselineSignature;    // 在役 SPV 接口签名（守卫基线，commit 后随进新值）
	};

	/** 守卫放行的重建候选：新 SPV + 其 hash/签名（commit 后写回基线，下轮守卫判据
	 *  = 最近一次已接受状态，而非启动时快照）。 */
	struct Candidate {
		uint32_t index;
		ShaderEffect effect;
		std::array<uint64_t, 2> newHash;
		std::array<std::string, 2> newSignature;
	};

	/** 全量重编 + 守卫 + 同槽重建（poll 检出变更时执行；返回是否有管线被重建）。 */
	bool reloadAll();
	/** 逐条目读在役 SPV 建基线（hash + 签名）；false = 有条目 SPV 不可读（已日志）。 */
	bool buildBaselines();
	/** 登记源目录全部 .slang 监视（v1：新文件不追登）；@return 监视文件数。 */
	int watchSources();
	/** 唯一源全量重编（共享 .slang 只 fork 一次）；false = 编译/产物读失败（已日志）。 */
	bool compileAll(std::map<std::string, std::vector<uint32_t>>& compiled);
	/** 逐条目守卫（字节无变跳过、契约变化 warn 拦下）；@return 接口兼容候选。 */
	std::vector<Candidate> collectCandidates(
		const std::map<std::string, std::vector<uint32_t>>& compiled);
	/** 帧边界换入：建替换管线 → waitForIdle → DGC 刷句柄 → commit → 刷基线。 */
	bool swapIn(const std::vector<Candidate>& candidates);

	ShaderRegistry& registry;
	Renderer& renderer;
	DgcCommands* dgc;
	DgcStaticCommands* dgcStatic;
	std::filesystem::path sourceDir;  // 源树 rgl/shaders/（RGL_SOURCE_SHADER_DIR）
	std::filesystem::path tempDir;    // 临时 SPV 输出（含 pid 级唯一后缀，析构清）
	HotReloader watcher;              // 源目录 .slang mtime 监视（回调只置 dirty）
	bool dirty = false;
	std::vector<std::array<StageSource, 2>> stageSources;  // 逐条目 {mesh, frag}，下标 = pipelineIndex
};

}  // namespace RGL
