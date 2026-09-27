module;
#include <filesystem>

export module hot_reload;

import rlstd;
import rllogger;

namespace RGL {

/**
 * 文件热重载监视器（Phase 10 批 10.3；docs/archive/phase10_plan.md §5）。
 *
 * 纯机制、零配置语义：登记「文件路径 + 变更回调」，`poll()`（帧边界调用，
 * 由 ConfigReloadHook 挂进帧循环）比对 mtime，变更即回调。语义（重载哪个配置、
 * 改哪些字段）全部在回调里，由装配方（main `wireHotReload`）决定——本模块
 * 不认识 scene/camera/logger。
 *
 * 未来 SPV/脚本热换共用同一入口（`脚本长期规划.md` 帧边界基建蓝图）。
 * 监视源 = 源树 configs/（D7：build 副本会被 `just build` 内容比对静默覆盖，
 * 监听它无意义）。
 */
export class HotReloader {
   public:
	/** 重载回调（参数 = 变更文件路径；契约 = 不得让异常逸出，本类 try/catch 兜底）。 */
	using ReloadFn = std::function<void(const std::string&)>;

	/**
	 * 登记监视文件。登记时文件不存在不算错（戳 = "从未见"；之后出现即触发
	 * 首次回调），仅记一行 info。
	 * @param file 文件路径（绝对路径——源树目录由编译期定义注入，见 main）
	 * @param onReload 变更回调（poll 时同步调用）
	 */
	void watch(std::filesystem::path file, ReloadFn onReload) {
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(file, ec);
		if (ec) {
			if (auto* logger = RLLogger::getInstance())
				logger->info("hot_reload: 登记时文件不可读（出现后才触发）: {}", file.string());
		}
		entries.push_back({std::move(file),
			ec ? std::optional<std::filesystem::file_time_type>{} : std::optional{stamp},
			std::move(onReload)});
	}

	/** 轮询变更（帧边界）：逐个条目比对 mtime 戳（判定逻辑见 pollEntry）。 */
	void poll() {
		for (Entry& e : entries)
			pollEntry(e);
	}

   private:
	/** 监视条目：路径 + 上次所见 mtime（nullopt = 文件尚不存在/不可读）+ 回调。 */
	struct Entry {
		std::filesystem::path file;
		std::optional<std::filesystem::file_time_type> last;
		ReloadFn onReload;
	};

	/**
	 * 单条目变更判定：mtime 与上次所见的戳不同 → 先更新戳再回调。
	 * 先更新 = 回调失败不重试（编辑器半写文件触发一次失败后，静默等下一次真变更，
	 * 不产生错误风暴）。文件消失 = 清戳 + warn 一次，再现时当新文件触发。
	 */
	void pollEntry(Entry& e) {
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(e.file, ec);
		if (ec) {
			if (e.last.has_value()) {
				if (auto* logger = RLLogger::getInstance())
					logger->warn("hot_reload: 监视文件消失: {}", e.file.string());
				e.last.reset();
			}
			return;
		}
		if (e.last && *e.last == stamp)
			return;
		e.last = stamp;
		try {
			e.onReload(e.file.string());
		} catch (const std::exception& ex) {
			if (auto* logger = RLLogger::getInstance())
				logger->warn("hot_reload: 回调抛出异常（已吞，保留旧值）: {} — {}",
					e.file.string(), ex.what());
		}
	}

	std::vector<Entry> entries;
};

}  // namespace RGL
