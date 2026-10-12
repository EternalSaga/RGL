module shader:impl;

import shader;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;

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

}  // namespace RGL
