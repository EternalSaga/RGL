module;
#include <sol/sol.hpp>
#include <fmt/format.h>
module diagnostics:impl;

import diagnostics;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;

namespace RGL {

// 诊断配置单例存储（进程级，main setDiagnostics 一次，此后只读）。
DiagnosticsConfig& diagnostics() {
	static DiagnosticsConfig instance;
	return instance;
}

void setDiagnostics(const DiagnosticsConfig& cfg) { diagnostics() = cfg; }

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

		// 数值门：负数 = 显式"关"哨兵（配置文件写 -1 直观）；越界钳位同旧 env 行为
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
		if (cfg.dumpGbuf > 3)
			cfg.dumpGbuf = 0;  // 越界钳到 0（同旧 env 行为）；负数 = 关

		if (auto* lg = RLLogger::getInstance())
			lg->info("diagnostics: no_vvl={} no_present={} vtx_canary={} texel_canary={} "
				"dump_gbuf={} style_dist={} tex_dist={} tex_force(size={} white={} unorm={})",
				cfg.noVvl, cfg.noPresent, cfg.vtxCanary,
				cfg.texelCanary == ~uint64_t{0} ? "off" : std::to_string(cfg.texelCanary),
				cfg.dumpGbuf, cfg.styleDist, cfg.texDist,
				cfg.textureForceSize, cfg.textureForceWhite, cfg.textureForceUnorm);
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("diagnostics config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
