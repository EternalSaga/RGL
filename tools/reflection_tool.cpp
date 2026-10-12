#include <boost/program_options.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

import rlstd;
import rllogger;
import schema_loader;
import validator;
import generator;
import slang_reflect;

namespace po = boost::program_options;
namespace fs = std::filesystem;
using json = nlohmann::json;

// SLANG_FLAGS 缺省值：与 rgl/CMakeLists.txt 逐字一致（CMake 恒显式传 --flags 覆盖，
// 此处缺省仅供 CLI 手跑；-fvk-use-scalar-layout 是布局关键，不可省）。
static const std::vector<std::string> kDefaultSlangFlags = {
	"-target", "spirv",
	"-capability", "spvDescriptorHeapEXT",
	"-profile", "sm_6_6+spirv_1_6",
	"-fvk-use-scalar-layout",
	"-no-codegen",
};

static void printUsage() {
	auto* logger = RGL::RLLogger::getInstance();
	logger->info("Usage:");
	logger->info("  reflection_tool validate  --json <path> --schema <path>");
	logger->info("  reflection_tool generate  --json <path> --schema <path> --output <path> --module-name <name> --template <path>");
	logger->info("  reflection_tool render     --json <path> --output <path> --template <path>");
	logger->info("  reflection_tool slang      --shader <path> [--shader <path>...] --schema <path> --output <path> --module-name <name> --template <path> [--flags <a b c ...>]");
}

int main(int argc, char* argv[]) {
	RGL::RLLogger::initialize(LOG_CONFIG_PATH);
	auto* logger = RGL::RLLogger::getInstance();
	logger->debug("reflection_tool started, pid={}", ::getpid());

	try {
		std::string command;
		std::string jsonPath, schemaPath, outputPath, moduleName, templatePath, slangFlagsStr;
		std::vector<std::string> shaderPaths, includePaths;

		po::options_description desc("Options");
		desc.add_options()("help,h", "produce help message")("json,j", po::value<std::string>(&jsonPath), "path to slangc reflection JSON")("schema,s", po::value<std::string>(&schemaPath), "path to Lua schema file")("output,o", po::value<std::string>(&outputPath), "output .cppm file path")("module-name,m", po::value<std::string>(&moduleName), "C++ module name for generated .cppm")("template,t", po::value<std::string>(&templatePath), "path to inja template file")("shader", po::value<std::vector<std::string>>(&shaderPaths), "slang source file(s) for 'slang' command (repeatable)")("flags", po::value<std::string>(&slangFlagsStr), "space-separated slang compiler flags for 'slang' (SLANG_FLAGS; boost 不吞 dash 参数故用单串 split)")("include,I", po::value<std::vector<std::string>>(&includePaths), "slang search path(s) for 'slang' (repeatable; -I)");

		if (argc < 2) {
			printUsage();
			return 1;
		}

		command = argv[1];
		int cmdArgc = argc - 1;
		char* cmdArgv[1024];
		std::ranges::copy_n(argv + 1, cmdArgc, cmdArgv);

		po::variables_map vm;
		po::store(po::command_line_parser(cmdArgc, cmdArgv).options(desc).run(), vm);
		po::notify(vm);

		if (vm.count("help")) {
			printUsage();
			return 0;
		}

		// slang 子命令不消费 JSON（直接链接 libslang.so 查 API）
		if (command == "slang") {
			if (shaderPaths.empty()) {
				logger->error("--shader is required for 'slang'");
				return 1;
			}
			if (vm["schema"].empty() || outputPath.empty() || moduleName.empty() || templatePath.empty()) {
				logger->error("'slang' requires --schema --output --module-name --template");
				return 1;
			}
			// flags：--flags 单串 split（缺省 = kDefaultSlangFlags）；-I 搜索路径追加
			std::vector<std::string> flags;
			if (slangFlagsStr.empty()) {
				flags = kDefaultSlangFlags;
			} else {
				std::string remaining = slangFlagsStr;
				size_t pos;
				while ((pos = remaining.find(' ')) != std::string::npos) {
					flags.push_back(remaining.substr(0, pos));
					remaining.erase(0, pos + 1);
				}
				if (!remaining.empty())
					flags.push_back(remaining);
			}
			for (const auto& inc : includePaths) {
				flags.push_back("-I");
				flags.push_back(inc);
			}
			RGL::Schema schema = RGL::loadSchema(schemaPath);
			RGL::SlangContext ctx;
			std::string diag;
			if (!ctx.compile(shaderPaths, flags, diag)) {
				logger->error("[FAIL] slang compile: {}", diag);
				return 1;
			}
			logger->info("[OK] slang compile succeeded, querying API");
			// 按 schema 顺序查询 + 交叉校验（vertexLayouts 先，pushConstants 后）
			std::vector<RGL::SlangStruct> structs;
			bool ok = true;
			// 批 R：嵌套契约 struct 名的合法判据 = 本 schema 的 vertexLayouts 名单
			// （vertexAttrCppType / validateSlangVertexLayout 与 generateCppmFromSlang 同源）
			std::set<std::string> contractStructs;
			for (const auto& vl : schema.vertexLayouts)
				if (!vl.name.empty())
					contractStructs.insert(vl.name);
			for (const auto& vl : schema.vertexLayouts) {
				if (vl.name.empty()) continue;
				auto api = ctx.queryStruct(vl.name);
				if (!api) {
					logger->error("[FAIL] vertexLayout '{}': struct not found in shader", vl.name);
					ok = false;
					continue;
				}
				if (!RGL::validateSlangVertexLayout(*api, vl, contractStructs)) ok = false;
				else {
					logger->info("[PASS] vertexLayout '{}' matches API (size={} fields={})",
						vl.name, api->size, api->fields.size());
					structs.push_back(std::move(*api));
				}
			}
			for (const auto& pc : schema.pushConstants) {
				if (pc.name.empty()) continue;
				auto api = ctx.queryPushConst(pc.name);
				if (!api) {
					logger->error("[FAIL] pushConst '{}': not found / not a constant buffer", pc.name);
					ok = false;
					continue;
				}
				if (!RGL::validateSlangPushConst(*api, pc)) ok = false;
				else {
					logger->info("[PASS] pushConst '{}' -> struct '{}' matches API (size={})",
						pc.name, api->name, api->size);
					structs.push_back(std::move(*api));
				}
			}
			if (!ok) {
				logger->error("[FAIL] slang cross-validation failed");
				return 1;
			}
			std::optional<std::string> cppm = RGL::generateCppmFromSlang(structs, schema, templatePath, moduleName);
			if (!cppm) {
				logger->error("[FAIL] Generation aborted");
				return 1;
			}
			std::ofstream out(outputPath);
			if (!out.is_open()) {
				logger->error("Cannot open output file: {}", outputPath);
				return 1;
			}
			out << *cppm;
			logger->info("[OK] Generated {} (slang API path)", outputPath);
			return 0;
		}

		// validate / generate / render 走 JSON 路径
		if (jsonPath.empty() || !fs::exists(jsonPath)) {
			logger->error("Reflection JSON not found: {}", jsonPath);
			return 1;
		}

		std::ifstream jsonFile(jsonPath);
		json reflection = json::parse(jsonFile);
		logger->debug("Loaded reflection JSON: {} parameters", reflection["parameters"].size());

		if (command == "validate") {
			if (vm["schema"].empty()) {
				logger->error("--schema is required for 'validate'");
				return 1;
			}
			auto schema = RGL::loadSchema(schemaPath);
			bool ok = RGL::validateSchema(reflection, schema);
			if (ok) {
				logger->info("[PASS] Schema validation passed");
				return 0;
			} else {
				logger->error("[FAIL] Schema validation failed");
				return 1;
			}

		} else if (command == "generate") {
			if (outputPath.empty()) {
				logger->error("--output is required for 'generate'");
				return 1;
			}
			if (moduleName.empty()) {
				logger->error("--module-name is required for 'generate'");
				return 1;
			}
			if (templatePath.empty()) {
				logger->error("--template is required for 'generate'");
				return 1;
			}

			// schema 必传：缺省 = 跳过校验直接生成（不安全模式，旧实现静默放行；
			// CMake 恒传 --schema，仅 CLI 误用会踩）
			if (vm["schema"].empty()) {
				logger->error("--schema is required for 'generate' (skips validation otherwise)");
				return 1;
			}
			RGL::Schema schema = RGL::loadSchema(schemaPath);
			if (!RGL::validateSchema(reflection, schema)) {
				logger->error("[FAIL] Schema validation failed before generation");
				return 1;
			}
			logger->info("[PASS] Schema validation passed, generating .cppm");

			std::optional<std::string> cppm = RGL::generateCppm(reflection, schema, templatePath, moduleName);
			if (!cppm) {
				logger->error("[FAIL] Generation aborted by layout validation — see [FAIL] entries");
				return 1;
			}
			std::ofstream out(outputPath);
			if (!out.is_open()) {
				logger->error("Cannot open output file: {}", outputPath);
				return 1;
			}
			out << *cppm;
			logger->info("[OK] Generated {}", outputPath);
			return 0;

		} else if (command == "render") {
			if (outputPath.empty()) {
				logger->error("--output is required for 'render'");
				return 1;
			}
			if (templatePath.empty()) {
				logger->error("--template is required for 'render'");
				return 1;
			}

			std::string cppm = RGL::renderTemplate(reflection, templatePath);
			std::ofstream out(outputPath);
			if (!out.is_open()) {
				logger->error("Cannot open output file: {}", outputPath);
				return 1;
			}
			out << cppm;
			logger->info("[OK] Rendered {}", outputPath);
			return 0;

		} else {
			logger->error("Unknown command: {}", command);
			printUsage();
			return 1;
		}

	} catch (const po::error& e) {
		logger->error("Error: {}", e.what());
		printUsage();
		return 1;
	} catch (const std::exception& e) {
		logger->error("Error: {}", e.what());
		return 1;
	}
}
