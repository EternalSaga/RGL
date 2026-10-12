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
};

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
			g.cppType = typeToCppType(p->getTypeLayout()->getType());
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
