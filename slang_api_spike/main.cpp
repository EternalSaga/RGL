// Slang C API 反射 spike：
// 调研目标——链接 libslang.so 经 C API 直接查询非资源 struct 的编译器实际布局
//（findTypeByName → getTypeLayout → 字段偏移），验证能否替代
// slangc CLI → JSON 中间层（现管线 vertexLayouts/constants 无反射端数据，纯 schema 手工信任）。
//
// 用法：slang_api_spike <shader> <structName> [<structName>...]
// flags 与 rgl/CMakeLists.txt SLANG_FLAGS 逐一对齐（scalar layout 是关键）。

#include <slang.h>

#include <cstdio>
#include <cstring>

namespace
{
	void dumpType(const char* indent, slang::TypeReflection* t)
	{
		char buf[192];
		switch (t->getKind())
		{
		case slang::TypeReflection::Kind::Scalar:
			snprintf(buf, sizeof buf, "scalar(%d)", (int)t->getScalarType());
			break;
		case slang::TypeReflection::Kind::Vector:
			snprintf(buf, sizeof buf, "vec%u<scalar %d>", (unsigned)t->getColumnCount(),
				(int)t->getScalarType());
			break;
		case slang::TypeReflection::Kind::Matrix:
			snprintf(buf, sizeof buf, "mat%ux%u", (unsigned)t->getRowCount(),
				(unsigned)t->getColumnCount());
			break;
		case slang::TypeReflection::Kind::Struct:
			snprintf(buf, sizeof buf, "struct %s", t->getName());
			break;
		default:
			snprintf(buf, sizeof buf, "kind=%d name=%s", (int)t->getKind(), t->getName());
			break;
		}
		printf("%s%s\n", indent, buf);
	}

	int dumpStruct(slang::ShaderReflection* refl, const char* name)
	{
		slang::TypeReflection* type = refl->findTypeByName(name);
		if (!type)
		{
			printf("[FAIL] findTypeByName(\"%s\") = null\n", name);
			return 1;
		}
		slang::TypeLayoutReflection* layout = refl->getTypeLayout(type);
		printf("struct %s: size=%zu align=%d fields=%u\n", name, layout->getSize(),
			layout->getAlignment(), layout->getFieldCount());
		for (unsigned i = 0; i < layout->getFieldCount(); i++)
		{
			slang::VariableLayoutReflection* f = layout->getFieldByIndex(i);
			slang::TypeLayoutReflection* ftl = f->getTypeLayout();
			printf("  [%u] %-10s offset=%-4zu size=%-3zu", i, f->getName(), f->getOffset(),
				ftl->getSize());
			dumpType("      ", ftl->getType());
		}
		return 0;
	}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: %s <shader> <structName> [<structName>...]\n", argv[0]);
		return 1;
	}
	const char* file = argv[1];

	slang::IGlobalSession* session = nullptr;
	SlangResult r = slang_createGlobalSession(SLANG_API_VERSION, &session);
	if (r != SLANG_OK)
	{
		fprintf(stderr, "[FAIL] createGlobalSession: %d\n", (int)r);
		return 1;
	}

	slang::ICompileRequest* req = nullptr;
	r = session->createCompileRequest(reinterpret_cast<SlangCompileRequest**>(&req));
	if (r != SLANG_OK)
	{
		fprintf(stderr, "[FAIL] createCompileRequest: %d\n", (int)r);
		return 1;
	}

	// 与 rgl/CMakeLists.txt SLANG_FLAGS 对齐（-fvk-use-scalar-layout 是布局关键）
	const char* cli[] = {
		file,
		"-target", "spirv",
		"-capability", "spvDescriptorHeapEXT",
		"-profile", "sm_6_6+spirv_1_6",
		"-fvk-use-scalar-layout",
		"-no-codegen",
	};
	r = req->processCommandLineArguments(cli, 9);
	if (r != SLANG_OK)
	{
		fprintf(stderr, "[FAIL] processCommandLineArguments: %d\n%s\n", (int)r,
			req->getDiagnosticOutput());
		return 1;
	}

	r = req->compile();
	if (r != SLANG_OK)
	{
		fprintf(stderr, "[FAIL] compile: %d\n%s\n", (int)r, req->getDiagnosticOutput());
		return 1;
	}

	// getReflection() 返回 C 式 opaque SlangReflection*；官方 helper 做 C→C++ 转换
	slang::ShaderReflection* refl = slang::ShaderReflection::get(req);
	if (!refl)
	{
		fprintf(stderr, "[FAIL] getReflection: null\n");
		return 1;
	}

	// 全局资源参数（binding/space）——对应现 JSON 的 descriptorSlots 段
	printf("global params: %u\n", refl->getParameterCount());
	for (unsigned i = 0; i < refl->getParameterCount(); i++)
	{
		slang::VariableLayoutReflection* p = refl->getParameterByIndex(i);
		printf("  [%u] %-12s binding=%u space=%u", i, p->getName(), p->getBindingIndex(),
			p->getBindingSpace());
		dumpType("      ", p->getTypeLayout()->getType());
	}

	int rc = 0;
	for (int i = 2; i < argc; i++)
		rc |= dumpStruct(refl, argv[i]);

	if (req)
		req->release();
	if (session)
		session->release();
	return rc;
}
