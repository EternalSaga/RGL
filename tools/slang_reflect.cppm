module;

#include <slang.h>
#include <fmt/format.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module slang_reflect;

import rlstd;
import rllogger;

namespace RGL {

// Slang C API 反射封装（替代 slangc CLI → JSON 中间层）。
// 调研结论见 docs/archive/slang_api_reflection_research.md。核心能力：查非资源 struct 的
// 编译器**实际**布局（size/align/字段偏移/字段类型，含跨模块）+ push constant 元素
// struct + 全局资源 binding。flags 与 rgl/CMakeLists.txt SLANG_FLAGS 逐字一致
//（-fvk-use-scalar-layout 是布局关键，经 processCommandLineArguments 原样传入）。

// 单字段：name + C++ 类型 + 编译器实际偏移/尺寸
export struct SlangField {
	std::string name;
	std::string cppType;
	uint32_t offset = 0;
	uint32_t size = 0;
};

// struct 布局（编译器实际算的，非 schema 声明）
export struct SlangStruct {
	std::string name;
	uint32_t size = 0;
	uint32_t align = 0;
	std::vector<SlangField> fields;
};

// 全局资源参数（对应现 JSON descriptorSlots 段，用于 binding 交叉校验）
export struct SlangGlobalParam {
	std::string name;
	uint32_t binding = 0;
	uint32_t space = 0;
	std::string cppType;
	// 读写方向（"read" / "readWrite" / "none" / …，见 accessToString）。
	// 立论：图外 compute 的同步对账要回答「这个 dispatch 会写哪些资源」，而人工申报的
	// 申报者与被判者同源（docs/compute_调研.md §7.10）——本字段是那条问题的**独立机器来源**。
	std::string access;
};

// SlangResourceAccess → 产物里的人读字符串（口径 → docs/compute_调研.md §7.11 的八种声明
// 映射表）。文件内 static：它是本模块的**编码细节**，导出它等于给下游一份第二口径，而下游
// 要判的是「可写集合」——那份集合归 tools/checks/config.py 登记，不在这里复制。
// 🔴 未识别的枚举值落 "unknown"，绝不映射成任何已知值：slang 升级新增 access 种类时，
// "unknown" 在产物里看得见、规则侧按非可写跳过（保守 = 少报同步边，但漂移可归因）；
// 猜成 readWrite 会凭空造边，猜成 read 会静默漏边——两个猜测都比「承认不知道」更坏。
static std::string accessToString(SlangResourceAccess a)
{
	switch (a) {
	case SLANG_RESOURCE_ACCESS_NONE: return "none";
	case SLANG_RESOURCE_ACCESS_READ: return "read";
	case SLANG_RESOURCE_ACCESS_READ_WRITE: return "readWrite";
	case SLANG_RESOURCE_ACCESS_RASTER_ORDERED: return "rasterOrdered";
	case SLANG_RESOURCE_ACCESS_APPEND: return "append";
	case SLANG_RESOURCE_ACCESS_CONSUME: return "consume";
	case SLANG_RESOURCE_ACCESS_WRITE: return "write";
	case SLANG_RESOURCE_ACCESS_FEEDBACK: return "feedback";
	default: return "unknown";
	}
}

	// 持有 slang 全局 session + ISession + 编译请求 + 反射（RAII，release 在析构）。
	// 仅 reflection_tool 一个 importer，且它链接 libslang.so——查询函数内联无妨。
	// 经 ISession::createCompileRequest（非 deprecated）而非 IGlobalSession 版
	//（IGlobalSession::createCompileRequest 已标 [[deprecated]]，2026.16）。
	export class SlangContext {
	public:
		SlangContext() = default;
		~SlangContext() {
			if (request)
				request->release();
			if (session)
				session->release();
			if (globalSession)
				globalSession->release();
		}
		SlangContext(const SlangContext&) = delete;
		SlangContext& operator=(const SlangContext&) = delete;

		// 编译 files（slang 源）+ flags（SLANG_FLAGS 同款）。失败 = false + diag 填诊断文本。
		// 反射在 compile 后即可用（query* 前须先 compile 成功）。
		bool compile(const std::vector<std::string>& files, const std::vector<std::string>& flags,
			std::string& diag)
		{
			auto* logger = RLLogger::getInstance();
			if (request) {
				logger->error("SlangContext::compile called twice");
				return false;
			}
			if (slang_createGlobalSession(SLANG_API_VERSION, &globalSession) != SLANG_OK) {
				diag = "slang_createGlobalSession failed";
				return false;
			}
			// 默认 SessionDesc（structureSize 由默认成员初始化器填）——与 spike 走
			// IGlobalSession::createCompileRequest 前的隐式默认会话等价
			slang::SessionDesc sessionDesc{};
			if (globalSession->createSession(sessionDesc, &session) != SLANG_OK) {
				diag = "createSession failed";
				return false;
			}
			if (session->createCompileRequest(reinterpret_cast<SlangCompileRequest**>(&request)) != SLANG_OK) {
				diag = "createCompileRequest failed";
				return false;
			}
		// CLI 参数 = files + flags，喂 processCommandLineArguments（与 slangc CLI 等价）
		cliArgs.clear();
		for (const auto& f : files)
			cliArgs.push_back(f.c_str());
		for (const auto& fl : flags)
			cliArgs.push_back(fl.c_str());
		std::string cliDiag;
		if (request->processCommandLineArguments(cliArgs.data(), (int)cliArgs.size()) != SLANG_OK) {
			cliDiag = request->getDiagnosticOutput() ? request->getDiagnosticOutput() : "";
			diag = "processCommandLineArguments failed: " + cliDiag;
			return false;
		}
		if (request->compile() != SLANG_OK) {
			cliDiag = request->getDiagnosticOutput() ? request->getDiagnosticOutput() : "";
			diag = "compile failed: " + cliDiag;
			return false;
		}
		reflection = slang::ShaderReflection::get(request);
		if (!reflection) {
			diag = "getReflection returned null";
			return false;
		}
		return true;
	}

	// 按名查 struct（跨模块可用）；未找到 = nullopt
	std::optional<SlangStruct> queryStruct(const std::string& name) const
	{
		if (!reflection)
			return std::nullopt;
		slang::TypeReflection* type = reflection->findTypeByName(name.c_str());
		if (!type || type->getKind() != slang::TypeReflection::Kind::Struct)
			return std::nullopt;
		return structFromLayout(reflection->getTypeLayout(type));
	}

	// 查 push constant（全局资源参数 varName）的元素 struct；非 CB/PB 或无元素 = nullopt
	std::optional<SlangStruct> queryPushConst(const std::string& varName) const
	{
		if (!reflection)
			return std::nullopt;
		for (unsigned i = 0; i < reflection->getParameterCount(); i++) {
			slang::VariableLayoutReflection* p = reflection->getParameterByIndex(i);
			if (std::string(p->getName()) != varName)
				continue;
			slang::TypeReflection* t = p->getTypeLayout()->getType();
			auto kind = t->getKind();
			if (kind != slang::TypeReflection::Kind::ConstantBuffer &&
				kind != slang::TypeReflection::Kind::ParameterBlock)
				return std::nullopt;
			slang::TypeReflection* elem = t->getElementType();
			if (!elem || elem->getKind() != slang::TypeReflection::Kind::Struct)
				return std::nullopt;
			return structFromLayout(reflection->getTypeLayout(elem));
		}
		return std::nullopt;
	}

	// 全局资源参数（binding/space，用于 descriptorSlots 交叉校验）
	std::vector<SlangGlobalParam> queryGlobalParams() const
	{
		std::vector<SlangGlobalParam> out;
		if (!reflection)
			return out;
		for (unsigned i = 0; i < reflection->getParameterCount(); i++) {
			slang::VariableLayoutReflection* p = reflection->getParameterByIndex(i);
			SlangGlobalParam g;
			g.name = p->getName();
			g.binding = p->getBindingIndex();
			g.space = p->getBindingSpace();
			// 🔴 getTypeLayout()/getType() 任一为空 → 填 "unknown"，**不解引用**：本产物是
			// 「哪个 dispatch 会写哪块资源」的唯一机器来源，一条空指针崩在 just reflect 里
			// 只留下一个栈回溯，而 "unknown" 会留在产物里被规则侧和人都看见。
			slang::TypeLayoutReflection* tl = p->getTypeLayout();
			slang::TypeReflection* t = tl ? tl->getType() : nullptr;
			g.access = accessToString(t ? t->getResourceAccess() : SLANG_RESOURCE_ACCESS_UNKNOWN);
			// 复用上面那个**已判空**的 t：`typeToCppType` 第一行就 `t->getKind()`，而本函数原先
			// 是 `p->getTypeLayout()->getType()` 裸取——同一个循环里新字段判空、旧字段不判，
			// 等于把崩溃路径留在自己刚立完护栏的旁边。（`queryPushConst` 等处的同型裸取是
			// 既有形态，不在本函数路径上，未一并改。）
			g.cppType = t ? typeToCppType(t) : std::string{};
			out.push_back(std::move(g));
		}
		return out;
	}

private:
	static std::optional<SlangStruct> structFromLayout(slang::TypeLayoutReflection* layout)
	{
		slang::TypeReflection* t = layout->getType();
		if (!t || t->getKind() != slang::TypeReflection::Kind::Struct)
			return std::nullopt;
		SlangStruct s;
		s.name = t->getName();
		s.size = (uint32_t)layout->getSize();
		s.align = (uint32_t)layout->getAlignment();
		for (unsigned i = 0; i < layout->getFieldCount(); i++) {
			slang::VariableLayoutReflection* f = layout->getFieldByIndex(i);
			slang::TypeLayoutReflection* ftl = f->getTypeLayout();
			SlangField fd;
			fd.name = f->getName();
			fd.cppType = typeToCppType(ftl->getType());
			fd.offset = (uint32_t)f->getOffset();
			fd.size = (uint32_t)ftl->getSize();
			s.fields.push_back(std::move(fd));
		}
		return s;
	}

	static std::string typeToCppType(slang::TypeReflection* t)
	{
		switch (t->getKind()) {
		case slang::TypeReflection::Kind::Scalar: {
			switch (t->getScalarType()) {
			case slang::TypeReflection::ScalarType::Float32: return "float";
			case slang::TypeReflection::ScalarType::Int32: return "int32_t";
			case slang::TypeReflection::ScalarType::UInt32: return "uint32_t";
			case slang::TypeReflection::ScalarType::Bool: return "bool";
			default: return "";
			}
		}
		case slang::TypeReflection::Kind::Vector: {
			std::string prefix;
			switch (t->getScalarType()) {
			case slang::TypeReflection::ScalarType::Int32: prefix = "i"; break;
			case slang::TypeReflection::ScalarType::UInt32: prefix = "u"; break;
			default: break;
			}
			return fmt::format("glm::{}vec{}", prefix, (unsigned)t->getColumnCount());
		}
		case slang::TypeReflection::Kind::Matrix: {
			unsigned rc = t->getRowCount(), cc = t->getColumnCount();
			if (rc == cc)
				return fmt::format("glm::mat{}", rc);
			return fmt::format("glm::mat{}x{}", rc, cc);
		}
		// 定长数组字段（★1-a2-i：cullPush.planes = float4[6]）→ std::array<元素, N>。
		// 元素类型**递归**走本函数；元素不可映射、或元素数拿不到（SLANG_UNBOUNDED_SIZE /
		// SLANG_UNKNOWN_SIZE）一律返回空串，让调用方既有的 unmappable 失败路径继续生效——
		// 🔴 不静默兜底成别的类型：兜底 = 生成物按错布局发字段而 [FAIL] 永远不响（本函数
		// default: return "" 存在的全部理由）。字符串形态与 schema 侧
		// schema_loader.cppm::vertexAttrCppType 的 "T[N]" 分支逐字符一致，交叉校验才成立。
		case slang::TypeReflection::Kind::Array: {
			const size_t n = t->getElementCount();
			if (n == SLANG_UNBOUNDED_SIZE || n == SLANG_UNKNOWN_SIZE)
				return "";
			const std::string elem = typeToCppType(t->getElementType());
			if (elem.empty())
				return "";
			return fmt::format("std::array<{}, {}>", elem, n);
		}
		// 批 R（迁移计划 §9.4 推论）：**嵌套契约 struct 字段** → C++ 类型名 = struct 名直出
		// （ObjectMatrix / GpuLight 这类惯用法的矩阵与灯都靠它嵌进 UBO 契约）。生成物与
		// 被依赖 struct 同文件发射，"依赖先出"由 generator.cppm::generateCppmFromSlang 校验。
		// 名字直取自 TypeReflection::getName()——**跨模块的 struct 也走这条路**（字段类型
		// 是已解析的 TypeReflection，不经过 findTypeByName 的名字查找；跨文件可用性实测 →
		// docs/constantbuffer_迁移计划.md §9.8 批 R 行）。拿不到名字（匿名 struct / 空名）
		// = 空串走既有 unmappable 失败路径——🔴 不兜底成别的类型：兜底 = 生成物按错布局
		// 发字段而 [FAIL] 永远不响（本函数 default: return "" 存在的全部理由）。
		case slang::TypeReflection::Kind::Struct: {
			const char* name = t->getName();
			return name ? std::string(name) : std::string();
		}
		default:
			return "";
		}
	}

		slang::IGlobalSession* globalSession = nullptr;
		slang::ISession* session = nullptr;
		slang::ICompileRequest* request = nullptr;
		slang::ShaderReflection* reflection = nullptr;
		mutable std::vector<const char*> cliArgs; // 须活过 processCommandLineArguments 调用
};

} // namespace RGL
