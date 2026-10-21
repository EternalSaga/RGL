module;
#include <fmt/format.h>  // fmt::format（compute binding 名解析失败的三类报错消息）
module shader:impl;

import shader;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import compute_bindings_generated;  // B2-1″：compute binding 名 → 号 接口表（构建期反射产物，rlbase）
import compute_access_log;  // B2-2：RgComputeBindingDecl / ComputeAccessLog（rlbase，纯值）
import offgraph_sync;      // B2-2：checkComputeHazard / syncEdgeDesc（同层 rlgfx；工厂是唯一
                             // 同时知道 (shader, param → 槽) 全事实的地方，所以挂点在这）
import rllogger;           // B2-2：hazard 类首次确认覆盖时的一行 info

namespace RGL {

namespace {

// SPIR-V opcode（仅签名解析器用到的；constexpr 常量而非裸 enum——style-check 红线）
constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kOpMemberDecorate = 72;
constexpr uint32_t kOpTypeVoid = 19;
constexpr uint32_t kOpTypeBool = 20;
constexpr uint32_t kOpTypeInt = 21;
constexpr uint32_t kOpTypeFloat = 22;
constexpr uint32_t kOpTypeVector = 23;
constexpr uint32_t kOpTypeMatrix = 24;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypeSampler = 26;
constexpr uint32_t kOpTypeSampledImage = 27;
constexpr uint32_t kOpTypeArray = 28;
constexpr uint32_t kOpTypeRuntimeArray = 29;
constexpr uint32_t kOpTypeStruct = 30;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpConstant = 43;
constexpr uint32_t kOpSpecConstant = 50;
constexpr uint32_t kOpVariable = 59;

// SPIR-V decoration 号（spvDeclaredBindings 判据用；签名解析器按设计不硬编码 decoration
// 语义——原始集相等即语义相等，本函数要的是 Binding 号本身，仅此一处点名）
constexpr uint32_t kDecorationBinding = 33;
/** Location 装饰号（fragment 输出接口判据 = spvFragmentOutputSignature；同上一处，语义单点）。 */
constexpr uint32_t kDecorationLocation = 30;

/** 指令流一遍切分的产物（类型/常量定义表 + decoration 表 + 接口变量清单）。 */
struct SpvModule {
	std::unordered_map<uint32_t, std::vector<uint32_t>> defs;  // 类型/常量 id → 指令词
	std::unordered_map<uint32_t,
		std::vector<std::pair<uint32_t, std::vector<uint32_t>>>> decs;  // id → (decoration, 操作数)
	std::unordered_map<uint32_t, std::vector<std::string>> memDecLines;  // struct id → MD 行
	std::vector<std::vector<uint32_t>> variables;
};

/** 冒号拼指令字面量行：prefix + inst[from..]（EM/MD 共用；成员/decoration/操作数
 *  语义由调用点决定，本函数只管拼）。 */
std::string spvLitLine(const std::string& prefix, const std::vector<uint32_t>& inst, size_t from) {
	std::string line = prefix;
	for (size_t k = from; k < inst.size(); ++k)
		line += ":" + std::to_string(inst[k]);
	return line;
}

/** OpEntryPoint token：EP{model}:{名字}（entry id 与 interface id 列表均漂移不取——
 *  接口面已由 decoration 变量 token 覆盖）。 */
std::string spvEpToken(const std::vector<uint32_t>& inst) {
	std::string tok = "EP" + std::to_string(inst[1]) + ":";
	bool nameDone = false;
	for (size_t k = 3; k < inst.size() && !nameDone; ++k) {
		for (int b = 0; b < 4; ++b) {
			char c = char((inst[k] >> (8 * b)) & 0xFFu);
			if (c == '\0') { nameDone = true; break; }  // 名字结束，后面是 interface id 列表
			tok += c;
		}
	}
	return tok;
}

/** 单条指令归类入库（spvSplit 的 switch 体拆出，降认知复杂度）。 */
void spvAbsorb(SpvModule& mod, std::vector<std::string>& tokens,
	const std::vector<uint32_t>& inst, uint32_t opcode) {
	switch (opcode) {
	case kOpDecorate:
		if (inst.size() >= 3)
			mod.decs[inst[1]].emplace_back(inst[2],
				std::vector<uint32_t>(inst.begin() + 3, inst.end()));
		return;
	case kOpEntryPoint:
		if (inst.size() >= 3)
			tokens.push_back(spvEpToken(inst));
		return;
	case kOpExecutionMode:
		// [1]=entry id（漂移，不取） [2]=mode [3..]=字面量操作数（LocalSize 等）
		if (inst.size() >= 3)
			tokens.push_back(spvLitLine("EM" + std::to_string(inst[2]), inst, 3));
		return;
	case kOpMemberDecorate:
		if (inst.size() >= 4)
			mod.memDecLines[inst[1]].push_back(
				spvLitLine("MD" + std::to_string(inst[2]) + ":" + std::to_string(inst[3]),
					inst, 4));
		return;
	case kOpVariable:
		mod.variables.push_back(inst);
		return;
	default:
		// 类型指令字 1 = 结果 id（操作数字面量自字 2 起）；常量字 1 = 类型、字 2 = 结果 id。
		// 索引起点错一格 = 查表全 miss → 原始 id 泄进签名 → 跨编译假漂移（实测踩坑）。
		if (opcode >= kOpTypeVoid && opcode <= kOpTypePointer) {
			if (inst.size() >= 2)
				mod.defs.emplace(inst[1], inst);
		}
		else if (opcode == kOpConstant || opcode == kOpSpecConstant) {
			if (inst.size() >= 3)
				mod.defs.emplace(inst[2], inst);
		}
		return;
	}
}

/** 指令流切分（header = 词 0..4，指令从词 5 起；指令词 0 = wordCount<<16|opcode）。
 *  false = 输入不是合法 SPIR-V（守卫侧以哨兵拒配）。 */
bool spvSplit(const std::vector<uint32_t>& spv, SpvModule& mod, std::vector<std::string>& tokens) {
	if (spv.size() < 6 || spv[0] != 0x07230203u)
		return false;
	for (size_t w = 5; w + 1 <= spv.size();) {
		const uint32_t word = spv[w];
		const uint32_t len = word >> 16, opcode = word & 0xFFFFu;
		if (len == 0 || w + len > spv.size())
			return false;
		const std::vector<uint32_t> inst(spv.begin() + static_cast<long>(w),
			spv.begin() + static_cast<long>(w + len));
		spvAbsorb(mod, tokens, inst, opcode);
		w += len;
	}
	return true;
}

/** id 的 decoration 行追加（类型级 + member 级，排序后原样拼入——**同型成员重排的
 *  可见性来源**：重排会改 Offset member decoration 的 member↔offset 配对集合）。 */
void spvAppendDecos(const SpvModule& mod, std::string& s, uint32_t id) {
	if (auto it = mod.decs.find(id); it != mod.decs.end()) {
		auto list = it->second;
		std::sort(list.begin(), list.end());
		for (const auto& [decKind, operands] : list) {
			s += " dec" + std::to_string(decKind);
			for (uint32_t o : operands)
				s += ":" + std::to_string(o);
		}
	}
	if (auto it = mod.memDecLines.find(id); it != mod.memDecLines.end()) {
		auto list = it->second;
		std::sort(list.begin(), list.end());
		for (const auto& line : list)
			s += " " + line;
	}
}

/** 类型 id → 结构 token（定义见下方；image 族分支互递归所以先行声明）。 */
std::string spvTypeToken(const SpvModule& mod, uint32_t id, int depth);

/** image 族（image/sampler/sampledImage）token：操作数字面量（字 3 起）+ 被采样类型
 *  （字 2）展开。OpTypeSampler 无操作数。 */
std::string spvImageToken(const SpvModule& mod, const std::vector<uint32_t>& d, int depth) {
	std::string s = ((d[0] & 0xFFFFu) == kOpTypeImage)
		? "I" : ((d[0] & 0xFFFFu) == kOpTypeSampler) ? "SA" : "SI";
	s += "(";
	for (size_t k = 2; k < d.size(); ++k)
		s += (k > 2 ? "," : "")
			+ ((k == 2) ? spvTypeToken(mod, d[2], depth + 1) : std::to_string(d[k]));
	return s + ")";
}

/** 类型 id → 结构 token（递归展开到字面量；类型 id 编译间漂移，必须展开到标量宽度/
 *  维数/成员序等字面量才可跨编译比较；类型操作数自字 2 起；类型图无环，深度上限纯防御）。 */
std::string spvTypeToken(const SpvModule& mod, uint32_t id, int depth) {
	if (depth > 24)
		return "?";
	auto it = mod.defs.find(id);
	if (it == mod.defs.end())
		return "id" + std::to_string(id);
	const auto& d = it->second;
	switch (d[0] & 0xFFFFu) {
	case kOpTypeVoid: return "v";
	case kOpTypeBool: return "b";
	case kOpTypeInt: return "i" + std::to_string(d[2]) + (d.size() > 3 && d[3] ? "s" : "u");
	case kOpTypeFloat: return "f" + std::to_string(d[2]);
	case kOpTypeVector: return "V" + spvTypeToken(mod, d[2], depth + 1) + "x" + std::to_string(d[3]);
	case kOpTypeMatrix: return "M" + spvTypeToken(mod, d[2], depth + 1) + "x" + std::to_string(d[3]);
	case kOpTypeArray: {
		// 元素 = 字 2，长度 = 常量 id（字 3）：整型常量取字面量词（常量字 3），否则 "?"
		auto len = mod.defs.find(d[3]);
		std::string n = (len != mod.defs.end() && len->second.size() >= 4)
			? std::to_string(len->second[3]) : "?";
		return "A[" + n + "]" + spvTypeToken(mod, d[2], depth + 1);
	}
	case kOpTypeRuntimeArray: return "RA" + spvTypeToken(mod, d[2], depth + 1);
	case kOpTypeStruct: {
		std::string s = "S(";
		for (size_t k = 2; k < d.size(); ++k)
			s += (k > 2 ? "," : "") + spvTypeToken(mod, d[k], depth + 1);
		s += ")";
		spvAppendDecos(mod, s, id);
		return s;
	}
	case kOpTypeImage:
	case kOpTypeSampler:
	case kOpTypeSampledImage:
		return spvImageToken(mod, d, depth);
	default: return "?" + std::to_string(d[0] & 0xFFFFu);
	}
}

/** 接口变量 token：storage class 原始号 + 原始 decoration 集全量（dec{值}:{操作数}
 *  ——Binding/DescriptorSet/Location 等语义无需硬编码，原始集相等即语义相等）+
 *  指向类型展开。空串 = 输入损坏。 */
std::string spvVarToken(const SpvModule& mod, const std::vector<uint32_t>& v) {
	if (v.size() < 4)
		return {};
	std::string tok = "V sc" + std::to_string(v[3]);
	if (auto dIt = mod.decs.find(v[2]); dIt != mod.decs.end()) {
		auto dlist = dIt->second;
		std::sort(dlist.begin(), dlist.end());
		for (const auto& [decKind, operands] : dlist) {
			tok += " dec" + std::to_string(decKind);
			for (uint32_t o : operands)
				tok += ":" + std::to_string(o);
		}
	}
	// v[1] = OpTypePointer 的 id；pointer 指令字 2 = 存储类、字 3 = 目标类型 id（取字 3；
	// 异常输入 → BADPTR 必不匹配）
	auto pIt = mod.defs.find(v[1]);
	if (pIt != mod.defs.end() && (pIt->second[0] & 0xFFFFu) == kOpTypePointer && pIt->second.size() >= 4)
		tok += " t" + spvTypeToken(mod, pIt->second[3], 0);
	else
		tok += " tBADPTR";
	return tok;
}

}  // namespace

/** spvInterfaceSignature 定义（契约见接口注释；实现拆 spvSplit/spvAbsorb/spvTypeToken/
 *  spvAppendDecos/spvVarToken，本函数只做组装）。 */
std::string spvInterfaceSignature(const std::vector<uint32_t>& spv) {
	SpvModule mod;
	std::vector<std::string> tokens;
	if (!spvSplit(spv, mod, tokens))
		return "SPV_CORRUPT";
	for (const auto& v : mod.variables) {
		auto tok = spvVarToken(mod, v);
		if (tok.empty())
			return "SPV_CORRUPT";
		tokens.push_back(std::move(tok));
	}
	std::sort(tokens.begin(), tokens.end());
	std::string sig;
	for (const auto& t : tokens) {
		if (!sig.empty())
			sig += ";";
		sig += t;
	}
	return sig;
}

/** spvDeclaredBindings 定义（契约见接口注释）。复用 spvSplit 的遍历与 decoration 采集
 *  （与 spvInterfaceSignature 同一套机制——两套解析器必然漂移）。判据 = 带 Binding(33)
 *  装饰的 OpVariable 的 binding 号：resource heap 与 sampler heap（OpTypeSampler 变量）
 *  同路采集；push constant 无 Binding 装饰天然排除。结构不认识（截断的 OpVariable、
 *  Binding 装饰无操作数）与 spvSplit 失败一样返回 nullopt——失败 ≠ 空集，调用方据此
 *  退回全烘。 */
std::optional<std::set<uint32_t>> spvDeclaredBindings(const std::vector<uint32_t>& spv) {
	SpvModule mod;
	std::vector<std::string> tokens;  // 本函数不消费签名 token，仅满足 spvSplit 出参
	if (!spvSplit(spv, mod, tokens))
		return std::nullopt;  // 魔数不对/截断 = 解析失败，不是「什么都不声明」
	std::set<uint32_t> bindings;
	for (const auto& v : mod.variables) {
		// OpVariable 字 2 = 结果 id（decs 键）；不足 4 词 = 结构不认识 → fail-safe
		if (v.size() < 4)
			return std::nullopt;
		auto it = mod.decs.find(v[2]);
		if (it == mod.decs.end())
			continue;  // 无 decoration 的变量（函数局部量等）
		for (const auto& [decKind, operands] : it->second) {
			if (decKind != kDecorationBinding)
				continue;
			if (operands.empty())
				return std::nullopt;  // Binding 装饰缺操作数 = 坏码流
			bindings.insert(operands[0]);
		}
	}
	return bindings;
}

namespace {

/** 单个 OpVariable → "loc<N>:<type>" 签名片段，非"带 Location 的输出变量"返回 nullopt。
 *  只收 storage class = Output(3) 且带 Location(30) 的变量：BuiltIn 输出（FragDepth 等）无
 *  Location，不参与 fragment output interface 匹配；无 OpTypePointer 的坏结构落 BADPTR
 *  （必不与任何合法签名相等）。 */
std::optional<std::string> spvOutputEntry(const SpvModule& mod, const std::vector<uint32_t>& v) {
	// OpVariable：字 1 = 结果类型、字 2 = 结果 id、字 3 = storage class（3 = Output）
	if (v.size() < 4 || v[3] != 3)
		return std::nullopt;
	std::optional<uint32_t> location;
	if (auto dIt = mod.decs.find(v[2]); dIt != mod.decs.end())
		for (const auto& [decKind, operands] : dIt->second)
			if (decKind == kDecorationLocation && !operands.empty())
				location = operands[0];
	if (!location)
		return std::nullopt;
	auto pIt = mod.defs.find(v[1]);
	const std::string type = (pIt != mod.defs.end()
		&& (pIt->second[0] & 0xFFFFu) == kOpTypePointer && pIt->second.size() >= 4)
		? spvTypeToken(mod, pIt->second[3], 0)
		: "BADPTR";
	return "loc" + std::to_string(*location) + ":" + type;
}

}  // namespace

/** spvFragmentOutputSignature 定义（契约见接口注释）。逐变量判据 → spvOutputEntry。 */
std::string spvFragmentOutputSignature(const std::vector<uint32_t>& spv) {
	SpvModule mod;
	std::vector<std::string> tokens;  // 本函数不消费签名 token，仅满足 spvSplit 出参
	if (!spvSplit(spv, mod, tokens))
		return "SPV_CORRUPT";
	std::vector<std::string> outs;
	for (const auto& v : mod.variables)
		if (auto entry = spvOutputEntry(mod, v))
			outs.push_back(*entry);
	std::sort(outs.begin(), outs.end());
	std::string sig;
	for (const auto& t : outs) {
		if (!sig.empty())
			sig += ";";
		sig += t;
	}
	return sig;
}

namespace {

/**
 * 报错消息用的名字清单拼接（逗号分隔）。契约见 resolveComputeBindings 的接口注释。
 *
 * 空清单写 `<无>`：消息里出现空清单会被读成"消息坏了"，而它真正的意思是"这一层是空的"
 * （接口表整条没生成 / 该 shader 一个 binding 都没有）——两种空必须能从消息里区分出来。
 */
std::string joinNames(const std::vector<std::string>& items) {
	std::string out;
	for (const auto& item : items)
		out += (out.empty() ? "" : ", ") + item;
	return out.empty() ? std::string{"<无>"} : out;
}

/** 报错消息用：接口表里有哪些 shader。 */
std::string knownComputeShaderNames() {
	std::vector<std::string> names;
	for (const auto& shader : computeShaderNames())
		names.emplace_back(shader);
	return joinNames(names);
}

/** 报错消息用：某 shader 的全部合法 binding 名（带号，省得再开 shader 查）。 */
std::string legalComputeBindingNames(std::string_view shader) {
	std::vector<std::string> names;
	for (const auto& b : computeBindings(shader))
		names.push_back(fmt::format("{}({})", b.name, b.binding));
	return joinNames(names);
}

}  // namespace

/** resolveComputeBinding 定义（契约见接口注释）。查的是构建期接口表，无设备、无反射。 */
std::optional<uint32_t>
resolveComputeBinding(std::string_view shader, std::string_view bindingName) {
	for (const auto& b : computeBindings(shader))
		if (b.name == bindingName)
			return b.binding;
	return std::nullopt;
}

/** computeShaderKey 定义（契约见接口注释）。不引 <filesystem>：只要 basename，两个斜杠字符够了。 */
std::string computeShaderKey(const std::string& spvPath) {
	const auto slash = spvPath.find_last_of("/\\");
	std::string key = slash == std::string::npos ? spvPath : spvPath.substr(slash + 1);
	if (std::string_view(key).ends_with(".spv"))
		key.resize(key.size() - 4);
	key += ".slang";
	return key;
}

/** resolveComputeBindings 定义（契约与三类报错的口径见接口注释）。 */
std::vector<std::pair<uint32_t, HeapSlot>> resolveComputeBindings(
	const std::string& spvPath,
	std::span<const std::pair<std::string_view, HeapSlot>> named) {
	const std::string shader = computeShaderKey(spvPath);
	// ① shader 身份先判：它不在表里时，后面每条名字的报错都会退化成"合法名：<无>"这种
	// 指错方向的消息（真因是这枚 shader 根本没进管线名单）。
	if (std::ranges::find(computeShaderNames(), std::string_view{shader})
	    == computeShaderNames().end())
		throw std::runtime_error(fmt::format(
			"makeMappedCompute: shader '{}' 不在 compute binding 接口表里（spvPath={}）。"
			"表由 compute-access 反射管线生成（名单见 cmake/RglReflection.cmake），表里现有：{}",
			shader, spvPath, knownComputeShaderNames()));
	std::vector<std::pair<uint32_t, HeapSlot>> resolved;
	resolved.reserve(named.size());
	for (const auto& [bindingName, heapSlot] : named) {
		const auto binding = resolveComputeBinding(shader, bindingName);
		// ② 名字不在该 shader 里：点名 shader + 错名 + 全部合法名（带号）
		if (!binding)
			throw std::runtime_error(fmt::format(
				"makeMappedCompute: binding 名 '{}' 不在 shader '{}' 的接口表里（spvPath={}）。"
				"该 shader 的合法 binding 名（号）：{}",
				bindingName, shader, spvPath, legalComputeBindingNames(shader)));
		// ③ 同名两次（等价于同 binding 两次）：两个堆槽抢一个 binding，后写的映射覆盖
		// 先写的 = 静默丢一个绑定
		const auto taken = std::ranges::find_if(resolved,
			[b = *binding](const auto& e) { return e.first == b; });
		if (taken != resolved.end())
			throw std::runtime_error(fmt::format(
				"makeMappedCompute: shader '{}' 的 binding 名 '{}' 传了两次（堆槽 {} 与 {}）"
				"——一个 binding 只能映射到一个堆槽",
				shader, bindingName, taken->second.slot, heapSlot.slot));
		resolved.emplace_back(*binding, heapSlot);
	}
	return resolved;
}

/** logComputeAccess 定义（四步契约见接口注释）。 */
void logComputeAccess(const std::string& spvPath,
	std::span<const std::pair<std::string_view, HeapSlot>> named, ComputeAccessLog& log) {
	const std::string shader = computeShaderKey(spvPath);
	// 名字 → 方向（同 resolveComputeBindings 查的那张表，同源 ⟹ 名字一定查得到）。
	// 查不到时按 none 记（两个 bool 都 false = 判据跳过这条）：这里**不**报错，因为「名字不在
	// 表里」上一句 resolveComputeBindings 已经抛过了，走到这里查不到只可能是实现坏了，
	// 而把它读成 hazard 会抛出一条指错方向的消息。真正的方向缺失在 compute_access.json 里可见。
	const auto table = computeBindings(shader);
	std::vector<RgComputeBindingDecl> decls;
	decls.reserve(named.size());
	for (const auto& [bindingName, heapSlot] : named) {
		const auto it = std::ranges::find_if(table,
			[&](const RgComputeBinding& b) { return b.name == bindingName; });
		decls.push_back(RgComputeBindingDecl{bindingName, heapSlot.slot,
			it == table.end() ? false : it->writable, it == table.end() ? false : it->readable});
	}
	for (const auto& hazard : log.add(shader, decls)) {
		const auto edge = checkComputeHazard(hazard);
		if (!edge)
			throw std::runtime_error(edge.error().describe());
		// 🔴 每类**首次**确认覆盖时打一行（add 已按类去重 ⟹ 天然只打一次）。这行日志是判据
		// **跑过**的证据：没有它，「锚命中」可能只是判据根本没跑（假绿三重里的第三重）。
		if (auto* logger = RLLogger::getInstance())
			logger->info("compute hazard: {}.{} ↔ {}.{}（heap 槽 {}）由边 '{}' 覆盖",
				hazard.shaderA, hazard.paramA, hazard.shaderB, hazard.paramB, hazard.slot,
				syncEdgeDesc(*edge).name);
	}
}

}  // namespace RGL
