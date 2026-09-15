module;
#include <sol/sol.hpp>
#include <fmt/format.h>
module diagnostics:impl;

import diagnostics;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;
import render_graph;  // kGBufferAttachments（dump_gbuf 越界闸门按表判界；render_graph 不依赖本模块，无环）

namespace RGL {

/** 诊断配置单例存储（进程级，main setDiagnostics 一次，此后只读）。 */
DiagnosticsConfig& diagnostics() {
	static DiagnosticsConfig instance;
	return instance;
}

/** setDiagnostics 定义：整体赋值到单例。 */
void setDiagnostics(const DiagnosticsConfig& cfg) { diagnostics() = cfg; }

/** loadDiagnosticsConfig 定义（接口注释见 diagnostics.cppm）。要点：数值门（texel_canary/dump_gbuf/texture_force_size）负数 = "关"哨兵；dump_gbuf 越界（> kGBufferAttachments 最大通道号）warn 一句并置 -1（关，不再静默改判为 0）；解析完成后打一行 info 汇总。 */
[[nodiscard]] std::expected<DiagnosticsConfig, Error> loadDiagnosticsConfig(std::string_view path) {
	auto* logger = RLLogger::getInstance();
	DiagnosticsConfig cfg;

	{
		std::ifstream probe{std::string(path)};
		if (!probe.is_open()) {
			if (logger)
				logger->warn("Diagnostics config not found ({}), all off", std::string(path));
			return cfg;
		}
	}

	sol::state lua;
	lua.open_libraries(sol::lib::base, sol::lib::table);
	lua["dofile"] = sol::nil;
	lua["loadfile"] = sol::nil;
	lua["package"] = sol::nil;
	lua["require"] = sol::nil;

	auto result = lua.do_file(std::string(path).c_str());
	if (!result.valid()) {
		sol::optional<sol::error> err = result.get<sol::optional<sol::error>>();
		std::string msg = err ? err->what() : std::string("unknown lua error");
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("diagnostics config ({}): {}", path, msg)});
	}

	try {
		sol::table root = result[0].as<sol::table>();
		auto readBool = [&](const char* key, bool def) {
			sol::optional<bool> v = root[key];
			return v ? *v : def;
		};

		cfg.noVvl = readBool("no_vvl", cfg.noVvl);
		cfg.noHdr = readBool("no_hdr", cfg.noHdr);
		cfg.noPresent = readBool("no_present", cfg.noPresent);
		cfg.vtxCanary = readBool("vtx_canary", cfg.vtxCanary);
		cfg.styleDist = readBool("style_dist", cfg.styleDist);
		cfg.texDist = readBool("tex_dist", cfg.texDist);
		cfg.textureForceWhite = readBool("texture_force_white", cfg.textureForceWhite);
		cfg.textureForceUnorm = readBool("texture_force_unorm", cfg.textureForceUnorm);
		cfg.dgcStatic = readBool("dgc_static", cfg.dgcStatic);

		// 数值门：负数 = 显式"关"哨兵（配置文件写 -1 直观）；dump_gbuf 越界 = warn + 关闭
		auto readI64 = [&](const char* key, int64_t def) -> std::optional<int64_t> {
			sol::optional<double> v = root[key];
			if (!v)
				return def;
			return static_cast<int64_t>(*v);
		};
		const int64_t texel = readI64("texel_canary", -1).value_or(-1);
		cfg.texelCanary = texel >= 0 ? static_cast<uint64_t>(texel) : ~uint64_t{0};
		cfg.textureForceSize = static_cast<uint32_t>(
			std::max<int64_t>(0, readI64("texture_force_size", 0).value_or(0)));
		cfg.dumpGbuf = static_cast<int>(readI64("dump_gbuf", -1).value_or(-1));
		constexpr int kMaxGbufChannel = static_cast<int>(kGBufferAttachments.size()) - 1;
		if (cfg.dumpGbuf > kMaxGbufChannel) {
			if (logger)
				logger->warn("diagnostics: dump_gbuf={} 超出 G-buffer 通道数（0..{}），已关闭",
					cfg.dumpGbuf, kMaxGbufChannel);
			cfg.dumpGbuf = -1;
		}
		cfg.frameStats = static_cast<uint32_t>(
			std::max<int64_t>(0, readI64("frame_stats", 0).value_or(0)));

		if (auto* lg = RLLogger::getInstance())
			lg->info("diagnostics: no_vvl={} no_present={} vtx_canary={} texel_canary={} "
				"dump_gbuf={} style_dist={} tex_dist={} tex_force(size={} white={} unorm={}) dgc_static={}",
				cfg.noVvl, cfg.noPresent, cfg.vtxCanary,
				cfg.texelCanary == ~uint64_t{0} ? "off" : std::to_string(cfg.texelCanary),
				cfg.dumpGbuf, cfg.styleDist, cfg.texDist,
				cfg.textureForceSize, cfg.textureForceWhite, cfg.textureForceUnorm, cfg.dgcStatic);
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("diagnostics config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
