module;
#include <chrono>     // steady_clock（tempDir 唯一后缀熵；GMF 例外清单成员）
#include <filesystem>

module shader_hot_reload:impl;

import shader_hot_reload;
import rlstd;
import rllogger;
import shader;  // loadShaderSpv + spvInterfaceSignature（守卫签名）
import shader_registry;
import vk_renderer;
import vk_indirect_commands;
import hot_reload;

namespace RGL {

namespace {

/** SPV 字节 FNV-1a（词粒度）：字节无变 = 免重建的快速判据。 */
uint64_t spvHash(std::span<const uint32_t> words) {
	uint64_t h = 0xcbf29ce484222325ull;
	for (const uint32_t w : words) {
		h ^= w & 0xFFu; h *= 0x100000001b3ull;
		h ^= (w >> 8) & 0xFFu; h *= 0x100000001b3ull;
		h ^= (w >> 16) & 0xFFu; h *= 0x100000001b3ull;
		h ^= (w >> 24) & 0xFFu; h *= 0x100000001b3ull;
	}
	return h;
}

/** spec SPV 相对路径（"generated_shaders/model.mesh.spv"）→ 源树 .slang 绝对路径
 *  （basename 换扩展，同 compile_slang 的 1:1 命名规则）。 */
std::filesystem::path slangOf(const std::string& spvRelPath, const std::filesystem::path& sourceDir) {
	std::string name = std::filesystem::path(spvRelPath).filename().string();
	if (name.size() > 4 && name.compare(name.size() - 4, 4, ".spv") == 0)
		name.replace(name.size() - 4, 4, ".slang");
	return sourceDir / name;
}

}  // namespace

ShaderHotReloader::ShaderHotReloader(ShaderRegistry& registry_, Renderer& renderer_,
	DgcCommands* dgc_, DgcStaticCommands* dgcStatic_)
	: registry(registry_), renderer(renderer_), dgc(dgc_), dgcStatic(dgcStatic_) {}

ShaderHotReloader::~ShaderHotReloader() {
	std::error_code ec;
	if (!tempDir.empty())
		std::filesystem::remove_all(tempDir, ec);
}

bool ShaderHotReloader::init() {
	auto* logger = RLLogger::getInstance();
#ifdef RGL_SLANGC_PATH
	std::error_code ec;
	if (!std::filesystem::exists(RGL_SLANGC_PATH, ec)) {
		if (logger)
			logger->warn("shader_hot_reload: slangc 不存在（热重载关）: {}", RGL_SLANGC_PATH);
		return false;
	}
	sourceDir = RGL_SOURCE_SHADER_DIR;
	if (!std::filesystem::is_directory(sourceDir, ec)) {
		if (logger)
			logger->warn("shader_hot_reload: 源 shaders 目录不存在（热重载关）: {}", sourceDir.string());
		return false;
	}
#else
	if (logger)
		logger->warn("shader_hot_reload: 编译期未注入 RGL_SLANGC_PATH（热重载关）");
	return false;
#endif
	if (!buildBaselines())
		return false;
	const int watched = watchSources();

	tempDir = std::filesystem::temp_directory_path() / ("rgl_shader_reload_"
		+ std::to_string(std::hash<std::string>{}(sourceDir.string())
			^ static_cast<size_t>(
				std::chrono::steady_clock::now().time_since_epoch().count())));
	std::filesystem::create_directories(tempDir, ec);

	if (logger)
		logger->info("shader_hot_reload: 已启动（L1 同签名热换），监视 {}（{} 个 .slang，"
			"变更 = 全量重编 ≈{} 条 fork，阻塞帧循环数秒）",
			sourceDir.string(), watched, watched);
	return true;
}

bool ShaderHotReloader::buildBaselines() {
	auto* logger = RLLogger::getInstance();
	for (const auto& entry : registry.entriesView()) {
		std::array<StageSource, 2> stages{};
		const std::array<const std::string*, 2> spvPaths{&entry.spec.meshSpv, &entry.spec.fragSpv};
		for (size_t s = 0; s < 2; ++s) {
			auto spv = RGL::loadShaderSpv(*spvPaths[s]);
			if (!spv) {  // 基线不全则守卫无从判起 → 降级关
				if (logger)
					logger->warn("shader_hot_reload: 基线 SPV 读取失败（热重载关）: {} — {}",
						*spvPaths[s], spv.error().message);
				return false;
			}
			stages[s].slang = slangOf(*spvPaths[s], sourceDir);
			stages[s].baselineHash = spvHash(*spv);
			stages[s].baselineSignature = spvInterfaceSignature(*spv);
		}
		stageSources.push_back(std::move(stages));
	}
	return true;
}

int ShaderHotReloader::watchSources() {
	int watched = 0;
	for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(sourceDir)) {
		if (dirEntry.is_directory() || dirEntry.path().extension() != ".slang")
			continue;
		watcher.watch(dirEntry.path(), [this](const std::string&) { dirty = true; });
		++watched;
	}
	return watched;
}

void ShaderHotReloader::poll() {
	watcher.poll();  // 回调只置 dirty（同帧多文件变更合并成一次重载）
	if (!dirty)
		return;
	dirty = false;
	reloadAll();
}

bool ShaderHotReloader::reloadAll() {
	std::map<std::string, std::vector<uint32_t>> compiled;
	if (!compileAll(compiled))
		return false;
	auto candidates = collectCandidates(compiled);
	if (candidates.empty()) {
		if (auto* logger = RLLogger::getInstance())
			logger->info("shader_hot_reload: 重编完成，无接口兼容的可热换变更（或被守卫拦下）");
		return false;
	}
	return swapIn(candidates);
}

bool ShaderHotReloader::compileAll(std::map<std::string, std::vector<uint32_t>>& compiled) {
#ifdef RGL_SLANGC_PATH
	auto* logger = RLLogger::getInstance();
	// 收集唯一源（共享 .slang 只 fork 一次）
	std::set<std::string> uniqueSources;
	for (const auto& stages : stageSources)
		for (const auto& st : stages)
			uniqueSources.insert(st.slang.string());
	for (const auto& src : uniqueSources) {
		const std::string outName = std::filesystem::path(src).filename().string() + ".spv";
		const std::filesystem::path outSpv = tempDir / outName;
		const std::string cmd = std::string("\"") + RGL_SLANGC_PATH + "\" \"" + src + "\" "
			+ RGL_SLANG_FLAGS + " -I \"" + sourceDir.string() + "\" -o \"" + outSpv.string() + "\" 2>&1";
		if (std::system(cmd.c_str()) != 0) {
			if (logger)
				logger->warn("shader_hot_reload: 编译失败（全部旧管线保留，等下次保存）: {}", src);
			return false;
		}
		auto spv = RGL::loadShaderSpv(outSpv.string());
		if (!spv) {
			if (logger)
				logger->warn("shader_hot_reload: 编译产物读取失败（旧管线保留）: {}", outSpv.string());
			return false;
		}
		compiled[src] = std::move(*spv);
	}
	return true;
#else
	(void)compiled;
	return false;  // init 已保证宏齐备才会到这里
#endif
}

std::vector<ShaderHotReloader::Candidate> ShaderHotReloader::collectCandidates(
	const std::map<std::string, std::vector<uint32_t>>& compiled) {
	auto* logger = RLLogger::getInstance();
	std::vector<Candidate> candidates;
	for (uint32_t i = 0; i < stageSources.size(); ++i) {
		const auto& stages = stageSources[i];
		// 🔴 **带 task stage 的条目不参与热重载**（★M-3T-a，docs/mdi_plan.md §19.16）：本工具只
		// 认 mesh + frag 两阶段，重建出的 ShaderEffect 会丢 taskSpv ⟹ RenderPipeline 退回
		// 「两条 createInfo + NO_TASK_SHADER_BIT」形态，而 mesh 那份 SPV 已经在等 payload
		// ⟹ payload 无生产者 = 静默画错。热重载与 `meshlet.mdi_shadow` 双双默认关，但"静默错"
		// 不在可接受形态里，故出声并保留旧管线（要热换 task 条目请重启）。
		if (!registry.entriesView()[i].spec.taskSpv.empty()) {
			if (logger)
				logger->info("shader_hot_reload: 跳过条目 {}（{}）——带 task stage，本工具只认 mesh+frag",
					i, registry.entriesView()[i].name);
			continue;
		}
		const auto& mesh = compiled.at(stages[0].slang.string());
		const auto& frag = compiled.at(stages[1].slang.string());
		const std::array<uint64_t, 2> hash{spvHash(mesh), spvHash(frag)};
		if (hash[0] == stages[0].baselineHash && hash[1] == stages[1].baselineHash)
			continue;  // 本条目两阶段字节无变
		const std::array<std::string, 2> sig{spvInterfaceSignature(mesh),
			spvInterfaceSignature(frag)};
		if (sig[0] != stages[0].baselineSignature || sig[1] != stages[1].baselineSignature) {
			if (logger)
				logger->warn("shader_hot_reload: 条目 {}（{}）接口契约变化——保留旧管线"
					"（同签名守卫 L1；完成契约迁移请重启）",
					i, registry.entriesView()[i].name);
			continue;  // 签名不符 = 契约变化被拦
		}
		candidates.push_back({i, ShaderEffect{mesh, frag}, hash, sig});
	}
	return candidates;
}

bool ShaderHotReloader::swapIn(const std::vector<Candidate>& candidates) {
	auto* logger = RLLogger::getInstance();
	// buildReplacement：旧管线仍活，单条目创建失败 = 该条目保留旧，其余继续。
	// swaps 存候选副本 + 新建管线（避免下方按下标对齐 candidates[k]——buildReplacement
	// 部分失败会让两个 vector 错位）。
	struct Swap {
		Candidate cand;
		RenderPipeline pipeline;
	};
	std::vector<Swap> swaps;
	swaps.reserve(candidates.size());
	for (const auto& cand : candidates) {
		try {
			swaps.push_back({cand, registry.buildReplacement(cand.index, cand.effect)});
		} catch (const std::exception& ex) {
			if (logger)
				logger->warn("shader_hot_reload: 条目 {} 新管线创建失败（保留旧）: {}",
					cand.index, ex.what());
		}
	}
	if (swaps.empty())
		return false;

	// 帧边界换入：排空 in-flight → DGC 槽指新句柄 → commit（旧句柄此刻销毁）
	renderer.waitForIdle();
	for (auto& swap : swaps) {
		const uint32_t index = swap.cand.index;
		if (dgc)
			dgc->updateShaderPair(index, swap.pipeline.fragShaderHandle(),
				swap.pipeline.meshShaderHandle());
		if (dgcStatic)
			dgcStatic->updateEntryShader(index, swap.pipeline.fragShaderHandle(),
				swap.pipeline.meshShaderHandle());
		registry.commitReplacement(index, std::move(swap.pipeline));
		// 刷基线 = 本候选的新 hash/签名（下轮守卫判据 = 最近一次已接受状态）
		stageSources[index][0].baselineHash = swap.cand.newHash[0];
		stageSources[index][1].baselineHash = swap.cand.newHash[1];
		stageSources[index][0].baselineSignature = swap.cand.newSignature[0];
		stageSources[index][1].baselineSignature = swap.cand.newSignature[1];
	}
	if (logger)
		logger->info("shader_hot_reload: 已重建 {} 条管线（pipelineIndex 不变，下一帧生效）",
			swaps.size());
	return true;
}

}  // namespace RGL
