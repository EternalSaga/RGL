// 债-140 §21/§22 独立最小复现器：task 驱动的 mesh 组总数 = 剂量轴。
// CTS 已证健康的形状做基线（经典管线、无 shader object/heap/DGC、新建缓冲），
// 逐维加回引擎特征：mdi / xsrc=buffer（数据驱动 Emit 组数）/ reuse=persist / frames 循环。
// 用法：repro --tasks 1530 --x 6 --frames 400 --mode direct --xsrc const
// 退出码：0 净（JSON 在 stdout）；3 = Vulkan/参数失败；4 = DEVICE_LOST（挂族信号）。
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
// 引导两函数直接从 libvulkan 导入（其余走 vkGetInstanceProcAddr）
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *);
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <fstream>

static void die(const char *what, VkResult r) {
	std::fprintf(stderr, "FATAL %s: VkResult=%d\n", what, (int)r);
	exit(3);
}
static void check(const char *what, VkResult r) { if (r != VK_SUCCESS) die(what, r); }

#define VKF_LIST \
	VKF(vkDestroyInstance) VKF(vkEnumeratePhysicalDevices) \
	VKF(vkGetPhysicalDeviceProperties) VKF(vkGetPhysicalDeviceFeatures2) \
	VKF(vkGetPhysicalDeviceQueueFamilyProperties) VKF(vkGetPhysicalDeviceMemoryProperties) \
	VKF(vkCreateDevice) VKF(vkGetDeviceQueue) \
	VKF(vkCreateShaderModule) \
	VKF(vkCreateRenderPass) \
	VKF(vkCreateImage) VKF(vkGetImageMemoryRequirements) \
	VKF(vkAllocateMemory) VKF(vkFreeMemory) VKF(vkBindImageMemory) \
	VKF(vkCreateImageView) VKF(vkCreateFramebuffer) \
	VKF(vkCreateGraphicsPipelines) VKF(vkCreatePipelineLayout) \
	VKF(vkCreateDescriptorSetLayout) VKF(vkCreateDescriptorPool) \
	VKF(vkAllocateDescriptorSets) VKF(vkUpdateDescriptorSets) \
	VKF(vkCreateBuffer) VKF(vkDestroyBuffer) VKF(vkGetBufferMemoryRequirements) \
	VKF(vkBindBufferMemory) VKF(vkMapMemory) VKF(vkFlushMappedMemoryRanges) VKF(vkUnmapMemory) \
	VKF(vkCreateCommandPool) VKF(vkAllocateCommandBuffers) \
	VKF(vkBeginCommandBuffer) VKF(vkEndCommandBuffer) \
	VKF(vkCmdBeginRenderPass) VKF(vkCmdEndRenderPass) VKF(vkCmdBindPipeline) \
	VKF(vkCmdSetViewport) VKF(vkCmdSetScissor) VKF(vkCmdBindDescriptorSets) \
	VKF(vkCmdPipelineBarrier) VKF(vkCmdCopyImageToBuffer) VKF(vkCmdDraw) \
	VKF(vkCmdDrawMeshTasksEXT) VKF(vkCmdDrawMeshTasksIndirectEXT) \
	VKF(vkQueueSubmit) VKF(vkDeviceWaitIdle) \
	VKF(vkCreateFence) VKF(vkWaitForFences) VKF(vkResetFences) \
	VKF(vkGetPhysicalDeviceProperties2) VKF(vkGetBufferDeviceAddress) \
	VKF(vkCreateShadersEXT) VKF(vkCmdBindShadersEXT) \
	VKF(vkCmdBindResourceHeapEXT) VKF(vkCmdPushDataEXT) VKF(vkWriteResourceDescriptorsEXT) \
	VKF(vkCmdBeginRendering) VKF(vkCmdEndRendering) VKF(vkCmdPushConstants) \
	VKF(vkCmdSetViewportWithCount) VKF(vkCmdSetScissorWithCount) VKF(vkCmdSetLineWidth) \
	VKF(vkCmdSetDepthBiasEnable) VKF(vkCmdSetBlendConstants) VKF(vkCmdSetDepthBounds) \
	VKF(vkCmdSetStencilCompareMask) VKF(vkCmdSetStencilWriteMask) VKF(vkCmdSetStencilReference) \
	VKF(vkCmdSetCullMode) VKF(vkCmdSetFrontFace) VKF(vkCmdSetPrimitiveTopology) \
	VKF(vkCmdSetPrimitiveRestartEnable) VKF(vkCmdSetRasterizerDiscardEnable) \
	VKF(vkCmdSetDepthTestEnable) VKF(vkCmdSetDepthWriteEnable) VKF(vkCmdSetDepthCompareOp) \
	VKF(vkCmdSetDepthBoundsTestEnable) VKF(vkCmdSetStencilTestEnable) VKF(vkCmdSetStencilOp) \
	VKF(vkCmdSetLogicOpEnableEXT) VKF(vkCmdSetLogicOpEXT) VKF(vkCmdSetVertexInputEXT) \
	VKF(vkCmdSetColorBlendEnableEXT) VKF(vkCmdSetColorBlendEquationEXT) \
	VKF(vkCmdSetColorWriteMaskEXT) VKF(vkCmdSetAlphaToCoverageEnableEXT) \
	VKF(vkCmdSetAlphaToOneEnableEXT) VKF(vkCmdSetPolygonModeEXT) \
	VKF(vkCmdSetRasterizationSamplesEXT) VKF(vkCmdSetSampleMaskEXT) \
	VKF(vkCmdSetDepthClampEnableEXT)
#define VKF(x) static PFN_##x x;
VKF_LIST
#undef VKF

struct Opts {
	uint32_t tasks = 1;      // direct: 每派发 task 数；mdi: 每行 taskCountX（引擎=1）
	uint32_t x = 1;          // 每 task Emit 的 mesh 组数
	uint32_t frames = 1;
	uint32_t dispatches = 1; // 每帧派发次数（引擎 = 3 级联）
	uint32_t rows = 0;       // mdi 行数（0 = 取 tasks）
	uint32_t stride = 12;    // mdi 行 stride（引擎 = 24）
	std::string mode = "direct"; // direct | mdi
	std::string xsrc = "const";  // const | buffer
	std::string reuse = "fresh"; // fresh | persist
	bool async = false;          // 帧间不等 fence（引擎 = 2-3 帧 in-flight）
	bool interleave = false;     // 每次 MDI 派发之间插经典 vertex 全屏 draw（引擎队列有别的活）
	bool heavy = false;          // mesh 三角形 = 全屏（每组执行时长↑）
	bool vary = false;           // 行 gcX ∈ {0,1} 随机 + xHost 2..18 随机（引擎孔洞流+可变组数）
	bool meshdrawid = false;     // M-1b 形状：mesh 入口读 DrawIndex
	bool meshread = false;       // 引擎形状：DI → 双间接内存链
	bool rebind = false;         // 每次派发前重绑管线（引擎 = 每级联重绑 shader object，DI 基址 SGPR 重设）
	bool notask = false;         // 真 M-1b 形状：mesh-only 管线（无 task 级），行 X = mesh 组数
	bool interdim2 = false;      // 候选②补强：直派臂用不同 meshlet 线程维度（DIM 寄存器重编程）
	uint32_t intermesh = 0;      // 候选②：MDI 之间插大型 mesh 直派（引擎主场景 27k 簇同量级）
	uint32_t intertask = 0;      // 候选②档②：插的直派臂 = task 驱动数据驱动组数（两路都吃 meshlet 机械）
	bool twopass = false;        // 候选②档③：两臂分属两个 render pass，共用同一 depth 靶（attachment 切换）
	// ---- §26 真取数链批（任务 13）----
	uint32_t chain = 0;          // 1 = 引擎形态 task + 真取数链 mesh；0 = 旧臂
	uint32_t triv = 0;           // 1 = 链 mesh 的 CHAIN=0 trivial 变体（R1 锚，配 --chain 1）
	bool heap = false;           // SO + descriptor heap 绑定（引擎形态）；false = 描述符集
	// 直派替代 MDI（引擎 S3 镜像）= `--mode direct`（链档下 grid X = 段行数）
	bool rowvary = false;        // 行表内容每帧改写（引擎逐帧剔除变化）
};

static const uint32_t W = 1024, H = 1024;
static std::vector<uint32_t> readSpv(const char *p) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	if (!f) { std::fprintf(stderr, "FATAL open %s\n", p); exit(3); }
	size_t n = (size_t)f.tellg();
	f.seekg(0);
	std::vector<char> buf(n);
	f.read(buf.data(), (std::streamsize)n);
	std::vector<uint32_t> out(n / 4);
	memcpy(out.data(), buf.data(), n);
	return out;
}

int main(int argc, char **argv) {
	Opts o;
	for (int i = 1; i < argc; ++i) {
		auto eq = [&](const char *k) { return strcmp(argv[i], k) == 0 && i + 1 < argc; };
		if (eq("--tasks")) o.tasks = (uint32_t)atoi(argv[++i]);
		else if (eq("--x")) o.x = (uint32_t)atoi(argv[++i]);
		else if (eq("--frames")) o.frames = (uint32_t)atoi(argv[++i]);
		else if (eq("--dispatches")) o.dispatches = (uint32_t)atoi(argv[++i]);
		else if (eq("--rows")) o.rows = (uint32_t)atoi(argv[++i]);
		else if (eq("--stride")) o.stride = (uint32_t)atoi(argv[++i]);
		else if (eq("--mode")) o.mode = argv[++i];
		else if (eq("--xsrc")) o.xsrc = argv[++i];
		else if (eq("--reuse")) o.reuse = argv[++i];
		else if (!strcmp(argv[i], "--async")) o.async = true;
		else if (!strcmp(argv[i], "--interleave")) o.interleave = true;
		else if (!strcmp(argv[i], "--heavy")) o.heavy = true;
		else if (!strcmp(argv[i], "--vary")) o.vary = true;
		else if (!strcmp(argv[i], "--meshdrawid")) o.meshdrawid = true;
		else if (!strcmp(argv[i], "--meshread")) o.meshread = true;
		else if (!strcmp(argv[i], "--rebind")) o.rebind = true;
		else if (!strcmp(argv[i], "--notask")) o.notask = true;
		else if (!strcmp(argv[i], "--interdim2")) o.interdim2 = true;
		else if (eq("--intermesh")) o.intermesh = (uint32_t)atoi(argv[++i]);
		else if (eq("--intertask")) o.intertask = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--twopass")) o.twopass = true;
		else if (eq("--chain")) o.chain = (uint32_t)atoi(argv[++i]);
		else if (eq("--triv")) o.triv = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--heap")) o.heap = true;
		else if (!strcmp(argv[i], "--rowvary")) o.rowvary = true;
		else { std::fprintf(stderr, "unknown arg %s\n", argv[i]); return 3; }
	}
	if (o.chain) {
		// §26 链档口径：行 stride 恒 24（引擎 MdiShadowRow），行数缺省 1500/段
		o.stride = 24;
		if (!o.rows) o.rows = 1500;
		if (o.mode != "mdi" && o.mode != "direct") return 3;
		if (o.heap && !o.chain) return 3; // heap 只在链档有意义（R1–R4 定义域）
	}
	const bool mdi = o.mode == "mdi", bufMode = o.xsrc == "buffer", persist = o.reuse == "persist";
	const uint32_t rows = (mdi || o.chain) ? (o.rows ? o.rows : o.tasks) : 1; // 链档 direct = DD grid
	const uint32_t tasksPerDispatch = mdi ? rows * o.tasks : o.tasks;
	const uint64_t totalGroups = (uint64_t)tasksPerDispatch * o.x * o.dispatches;

	// ---- instance / device ----
	VkApplicationInfo app{};
	app.apiVersion = VK_API_VERSION_1_3;
	app.pApplicationName = "repro";
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = 0;
	ici.ppEnabledExtensionNames = nullptr;
	VkInstance inst;
	check("CreateInstance", vkCreateInstance(&ici, nullptr, &inst));
#define VKF(x) x = (PFN_##x)vkGetInstanceProcAddr(inst, #x); if (!x) { std::fprintf(stderr, "missing " #x "\n"); return 3; }
	VKF_LIST
#undef VKF
	std::vector<VkPhysicalDevice> pds(16);
	uint32_t npd = 16;
	{
		VkResult r = vkEnumeratePhysicalDevices(inst, &npd, pds.data());
		if (r != VK_SUCCESS && r != VK_INCOMPLETE) die("EnumPD", r);
	}
	pds.resize(npd);
	if (pds.empty()) die("EnumPD", VK_ERROR_INITIALIZATION_FAILED);
	VkPhysicalDevice pd = pds[0];
	for (auto c : pds) {
		VkPhysicalDeviceProperties pr;
		vkGetPhysicalDeviceProperties(c, &pr);
		if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = c; break; }
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	VkPhysicalDeviceFeatures2 feats2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	VkPhysicalDeviceMeshShaderFeaturesEXT msf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
	msf.pNext = feats2.pNext;
	feats2.pNext = &msf;
	vkGetPhysicalDeviceFeatures2(pd, &feats2);
	if (!msf.meshShader) { std::fprintf(stderr, "FATAL meshShader feature unsupported\n"); return 3; }
	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
	std::vector<VkQueueFamilyProperties> qf(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
	uint32_t qfi = 0;
	for (uint32_t i = 0; i < nq; ++i)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
	float qa = 1.0f;
	VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	dq.queueFamilyIndex = qfi;
	dq.queueCount = 1;
	dq.pQueuePriorities = &qa;
	msf.pNext = nullptr;
	memset(&msf, 0, sizeof(msf));
	msf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
	msf.meshShader = VK_TRUE;
	msf.taskShader = VK_TRUE; // 只开这两枚（查询块原样回传会连带开 multiview 等未启依赖）
	VkPhysicalDeviceVulkan11Features v11f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	v11f.shaderDrawParameters = VK_TRUE; // gl_DrawID 的 spec 前提（引擎同款）
	VkPhysicalDeviceVulkan12Features v12f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	v12f.drawIndirectCount = VK_TRUE;
	v12f.runtimeDescriptorArray = VK_TRUE;
	v12f.descriptorIndexing = VK_TRUE;   // 引擎同款开满（绑定模型尽量不输）
	v12f.bufferDeviceAddress = VK_TRUE;  // §26 heap 档：描述符 = buffer 设备地址
	v12f.pNext = &msf;
	v11f.pNext = &v12f;
	// §26 heap 档：ESO + dynamic rendering + descriptor heap（引擎三件套，CTS 合并测试同款）
	VkPhysicalDeviceVulkan13Features v13f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	v13f.dynamicRendering = VK_TRUE;
	v13f.pNext = &v11f;
	VkPhysicalDeviceDescriptorHeapFeaturesEXT heapF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
	heapF.descriptorHeap = VK_TRUE;
	VkPhysicalDeviceShaderObjectFeaturesEXT soF{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
	soF.shaderObject = VK_TRUE;
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3F{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
	// ESO 基线状态块用到的五枚（VUID-08400 同族前提）
	eds3F.extendedDynamicState3ColorWriteMask = VK_TRUE;
	eds3F.extendedDynamicState3ColorBlendEnable = VK_TRUE;
	eds3F.extendedDynamicState3ColorBlendEquation = VK_TRUE;
	eds3F.extendedDynamicState3AlphaToOneEnable = VK_TRUE;
	eds3F.extendedDynamicState3LogicOpEnable = VK_TRUE;
	heapF.pNext = &soF;
	soF.pNext = &eds3F;
	eds3F.pNext = &v13f;
	feats2.features = {};
	feats2.features.multiDrawIndirect = VK_TRUE; // 🔴 core 特性：缺了它 drawCount>1 非法（VUID-02718）
	feats2.pNext = o.heap ? (void *)&heapF : (void *)&v13f;
	const char *dext[] = {VK_EXT_MESH_SHADER_EXTENSION_NAME, VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
		VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
		VK_KHR_MAINTENANCE_5_EXTENSION_NAME}; // heap 依赖 maintenance5（01387 实测点名）
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &feats2;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &dq;
	dci.enabledExtensionCount = o.heap ? 5u : 1u;
	dci.ppEnabledExtensionNames = dext;
	dci.pEnabledFeatures = nullptr; // features2 在 pNext 链上，二者不得同给
	VkDevice dev;
	check("CreateDevice", vkCreateDevice(pd, &dci, nullptr, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, qfi, 0, &queue);

	// ---- 内存/缓冲工具 ----
	auto memIdx = [&](uint32_t typeBits, VkMemoryPropertyFlags want) {
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
			if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
				return i;
		die("memType", VK_ERROR_FEATURE_NOT_PRESENT);
		return 0u;
	};
	auto makeImage = [&](VkFormat fmt, VkImageUsageFlags usage, VkImage &img, VkDeviceMemory &mem) {
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
		ii.extent = {W, H, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = usage; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		check("CreateImage", vkCreateImage(dev, &ii, nullptr, &img));
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(dev, img, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = memIdx(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		check("AllocImageMem", vkAllocateMemory(dev, &ai, nullptr, &mem));
		check("BindImage", vkBindImageMemory(dev, img, mem, 0));
	};
	auto makeView = [&](VkImage img, VkFormat fmt, VkImageAspectFlags aspect) {
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
		vi.subresourceRange = {aspect, 0, 1, 0, 1};
		VkImageView v;
		check("CreateView", vkCreateImageView(dev, &vi, nullptr, &v));
		return v;
	};
	auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &b, VkDeviceMemory &m, void **mapOut) {
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		check("CreateBuffer", vkCreateBuffer(dev, &bi, nullptr, &b));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(dev, b, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = memIdx(mr.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		check("AllocBufMem", vkAllocateMemory(dev, &ai, nullptr, &m));
		check("BindBuffer", vkBindBufferMemory(dev, b, m, 0));
		if (mapOut) check("MapMemory", vkMapMemory(dev, m, 0, size, 0, mapOut));
	};
	auto flushMem = [&](VkDeviceMemory m, VkDeviceSize) {
		VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		r.memory = m; r.size = VK_WHOLE_SIZE; // 整段刷（避开 nonCoherentAtomSize 对齐 VUID）
		vkFlushMappedMemoryRanges(dev, 1, &r);
	};

	// ---- 离屏 ----
	const VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM, dfmt = VK_FORMAT_D32_SFLOAT;
	VkImage colorImg, depthImg;
	VkDeviceMemory colorMem, depthMem;
	makeImage(cfmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, colorImg, colorMem);
	makeImage(dfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depthImg, depthMem);
	VkImageView colorVw = makeView(colorImg, cfmt, VK_IMAGE_ASPECT_COLOR_BIT);
	VkImageView depthVw = makeView(depthImg, dfmt, VK_IMAGE_ASPECT_DEPTH_BIT);
	std::array<VkImageView, 2> views{colorVw, depthVw};
	VkAttachmentDescription att[2]{};
	att[0] = {0, cfmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	att[1] = {0, dfmt, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
	VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub{};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &colorRef;
	sub.pDepthStencilAttachment = &depthRef;
	VkSubpassDependency dep{};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpi.attachmentCount = 2; rpi.pAttachments = att;
	rpi.subpassCount = 1; rpi.pSubpasses = &sub;
	rpi.dependencyCount = 1; rpi.pDependencies = &dep;
	VkRenderPass rp;
	check("CreateRP", vkCreateRenderPass(dev, &rpi, nullptr, &rp));
	VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fbi.renderPass = rp;
	fbi.attachmentCount = 2;
	fbi.pAttachments = views.data();
	fbi.width = W; fbi.height = H; fbi.layers = 1;
	VkFramebuffer fb;
	check("CreateFB", vkCreateFramebuffer(dev, &fbi, nullptr, &fb));

	// ---- 管线 ----
	auto loadMod = [&](const char *path) {
		auto code = readSpv(path);
		VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		mi.codeSize = code.size() * 4;
		mi.pCode = code.data();
		VkShaderModule m;
		check("CreateSM", vkCreateShaderModule(dev, &mi, nullptr, &m));
		return m;
	};
	VkShaderModule taskM = loadMod(bufMode ? "shaders/task_buffer.spv" : "shaders/task_const.spv");
	// 🔴 gl_DrawID 死代码也算静态使用（09631 实证）→ DI 与否是两个编译变体，不是 spec 开关
	VkShaderModule meshM = loadMod((o.meshdrawid || o.meshread) ? "shaders/mesh_di.spv" : "shaders/mesh.spv");
	VkShaderModule meshPlainM = (o.intermesh && meshM != VK_NULL_HANDLE) ? loadMod("shaders/mesh.spv") : VK_NULL_HANDLE;
	VkShaderModule fragM = loadMod("shaders/frag.spv");
	const uint32_t specX = o.x, specH = o.heavy ? 1u : 0u, specD = o.meshdrawid ? 1u : 0u,
					 specR = o.meshread ? 1u : 0u;
	VkSpecializationMapEntry sme{0, 0, sizeof(specX)};
	VkSpecializationInfo spec{};
	spec.mapEntryCount = 1;
	spec.pMapEntries = &sme;
	spec.dataSize = sizeof(specX);
	spec.pData = &specX;
	std::array<VkSpecializationMapEntry, 3> smeH{VkSpecializationMapEntry{0, 0, sizeof(specH)},
		VkSpecializationMapEntry{1, sizeof(specH), sizeof(specD)},
		VkSpecializationMapEntry{2, sizeof(specH) + sizeof(specD), sizeof(specR)}};
	uint32_t specHPayload[3] = {specH, specD, specR};
	VkSpecializationInfo specH2{};
	specH2.mapEntryCount = 3;
	specH2.pMapEntries = smeH.data();
	specH2.dataSize = sizeof(specHPayload);
	specH2.pData = specHPayload;
	std::array<VkPipelineShaderStageCreateInfo, 3> stg{};
	stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stg[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT; stg[0].module = taskM; stg[0].pName = "main";
	if (!bufMode) stg[0].pSpecializationInfo = &spec;
	stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stg[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT; stg[1].module = meshM; stg[1].pName = "main";
	stg[1].pSpecializationInfo = &specH2;
	stg[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stg[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[2].module = fragM; stg[2].pName = "main";
	VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1; cb.pAttachments = &cba;
	VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
	VkDescriptorSetLayoutBinding dsb[2]{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_TASK_BIT_EXT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr}};
	VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dsli.bindingCount = 2; dsli.pBindings = dsb; // mesh.glsl 恒声明 rows_（binding 1）→ 布局恒两枚
	VkDescriptorSetLayout dsl;
	check("CreateDSL", vkCreateDescriptorSetLayout(dev, &dsli, nullptr, &dsl));
	std::array<VkDescriptorSetLayout, 4> dsls{dsl, dsl, dsl, dsl};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1; pli.pSetLayouts = &dsl;
	VkPipelineLayout pl;
	check("CreatePL", vkCreatePipelineLayout(dev, &pli, nullptr, &pl));
	std::array<VkPipelineShaderStageCreateInfo, 3> stgC{}; // mesh-only 管线（无 task）
	const std::array<VkPipelineShaderStageCreateInfo, 3> stgAll = stg;
	if (o.notask) {
		stgC[0] = stgAll[1]; // mesh
		stgC[1] = stgAll[2]; // frag
	}
	const auto stgUse = o.notask ? stgC : stgAll;
	VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpi.stageCount = o.notask ? 2u : 3u;
	gpi.pStages = stgUse.data();
	gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp;
	gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms; gpi.pDepthStencilState = &ds;
	gpi.pColorBlendState = &cb; gpi.pDynamicState = &dsi;
	gpi.layout = pl; gpi.renderPass = rp; gpi.subpass = 0;
	VkPipeline pipe;
	check("CreatePipe", vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));

	// ---- 第二管线（interleave 用经典顶点管线，同 render pass）----
	VkPipeline pipeV = VK_NULL_HANDLE;
	if (o.interleave) {
		VkShaderModule vertM = loadMod("shaders/vert.spv");
		VkPipelineShaderStageCreateInfo vstg[2]{};
		vstg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		vstg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; vstg[0].module = vertM; vstg[0].pName = "main";
		vstg[1] = stg[2]; // frag
		VkGraphicsPipelineCreateInfo vgpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		vgpi.stageCount = 2; vgpi.pStages = vstg;
		vgpi.pVertexInputState = &vin; vgpi.pInputAssemblyState = &ia; vgpi.pViewportState = &vp;
		vgpi.pRasterizationState = &rs; vgpi.pMultisampleState = &ms; vgpi.pDepthStencilState = &ds;
		vgpi.pColorBlendState = &cb; vgpi.pDynamicState = &dsi;
		vgpi.layout = pl; vgpi.renderPass = rp; vgpi.subpass = 0;
		check("CreatePipeV", vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &vgpi, nullptr, &pipeV));
	}

	// ---- 候选②：大型 mesh 直派管线（无 task、无 DI = 引擎主场景 meshlet 直派的机械形态）----
	VkPipeline pipeM = VK_NULL_HANDLE;
	if (o.intermesh) {
		uint32_t zero3[3] = {0, 0, 0}; // 直派臂不吃 HEAVY/DRAWID/MESHREAD
		static VkSpecializationMapEntry smeM[3]{{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
		VkSpecializationInfo specM{};
		specM.mapEntryCount = 3; specM.pMapEntries = smeM;
		specM.dataSize = sizeof(zero3);
		static uint32_t zeroPayload[3];
		memcpy(zeroPayload, zero3, sizeof(zero3));
		specM.pData = zeroPayload;
		VkPipelineShaderStageCreateInfo mstg[2] = {stg[1], stg[2]};
		mstg[0].pSpecializationInfo = &specM;
		mstg[0].module = o.interdim2 ? loadMod("shaders/mesh_dim2.spv") : meshPlainM;
		if (o.interdim2) mstg[0].pSpecializationInfo = nullptr; // dim2 无 spec 常量
		VkGraphicsPipelineCreateInfo mgpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		mgpi.stageCount = 2; mgpi.pStages = mstg;
		mgpi.pVertexInputState = &vin; mgpi.pInputAssemblyState = &ia; mgpi.pViewportState = &vp;
		mgpi.pRasterizationState = &rs; mgpi.pMultisampleState = &ms; mgpi.pDepthStencilState = &ds;
		mgpi.pColorBlendState = &cb; mgpi.pDynamicState = &dsi;
		mgpi.layout = pl; mgpi.renderPass = rp; mgpi.subpass = 0;
		check("CreatePipeM", vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &mgpi, nullptr, &pipeM));
	}

	// ---- 候选②档③：第二 render pass（同 color+depth 靶 = attachment 切换）----
	VkRenderPass rp2 = VK_NULL_HANDLE;
	VkFramebuffer fb2 = VK_NULL_HANDLE;
	if (o.twopass) {
		VkAttachmentDescription att2[2] = {att[0], att[1]};
		att2[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att2[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		att2[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att2[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att2[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		VkRenderPassCreateInfo r2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		r2.attachmentCount = 2; r2.pAttachments = att2;
		r2.subpassCount = 1; r2.pSubpasses = &sub;
		r2.dependencyCount = 1; r2.pDependencies = &dep;
		check("CreateRP2", vkCreateRenderPass(dev, &r2, nullptr, &rp2));
		VkFramebufferCreateInfo f2{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		f2.renderPass = rp2; f2.attachmentCount = 2; f2.pAttachments = views.data();
		f2.width = W; f2.height = H; f2.layers = 1;
		check("CreateFB2", vkCreateFramebuffer(dev, &f2, nullptr, &fb2));
	}

	constexpr uint32_t kRing = 4; // ring×4（引擎 ring×2 的近似上界，帧间不竞态口径同旧臂）
	// ---- §26 真取数链批：链数据（全合法界内）+ 缓冲 + heap/SO 与描述符集两种绑定 ----
	// 数据形状 = 引擎五级链：行表(24B) → 对象区间表 → 簇条目(44B) → Tris(u8)/Remap(u32) → 顶点池(48B)。
	// 🔴 §23.4 红线：数据全部合法、界内、自洽——引擎就是数据全对也崩，本档不造越界。
	const uint32_t NOBJ = 64, CPER = 16; // 对象数 × 每对象簇数（区间表自洽）
	uint32_t totalVerts = 0;
	std::vector<uint32_t> rangeHost, clusterHost, trisHostW, remapHost, poolHost;
	VkDeviceSize trisBytes = 0;
	auto f2u = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
	uint64_t rowSeed = 0xC0FFEE140ull; // 行表 LCG（rowvary 跨帧续流 = 引擎逐帧剔除变化）
	auto rowRnd = [&]() {
		rowSeed = (rowSeed * 6364136223846793005ull + 1442695040888963407ull) >> 16;
		return (uint32_t)(rowSeed >> 17);
	};
	const uint32_t segs = o.dispatches;
	const uint32_t rowsPerSlot = rows * segs; // 行表按 ring 槽分段（引擎 sliceRows 同构）
	auto fillRows = [&](void *dst) { // 24B/行 = {gcX,1,1,obj,cstart,mgc}（引擎 MdiShadowRow）
		uint32_t *w = (uint32_t *)dst;
		for (uint32_t r = 0; r < rowsPerSlot; ++r) {
			w[r * 6u + 0] = (rowRnd() % 10u) < 8u ? 1u : 0u; // gcX：80% 可见（孔洞流）
			w[r * 6u + 1] = 1u;
			w[r * 6u + 2] = 1u;
			w[r * 6u + 3] = rowRnd() % NOBJ;            // obj
			w[r * 6u + 4] = rowRnd() % 8u;              // cstart ∈ [0,8)
			w[r * 6u + 5] = 2u + rowRnd() % 7u;         // mgc ∈ [2,8]：cstart+(mgc-1)/2 ≤ 10 < CPER 界内
		}
	};
	// 带 DEVICE_ADDRESS 的缓冲（heap 描述符的裸地址来源）
	auto makeBufferAddr = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &b,
		VkDeviceMemory &m, void **mapOut) {
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = size;
		bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		check("CreateBufferA", vkCreateBuffer(dev, &bi, nullptr, &b));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(dev, b, &mr);
		VkMemoryAllocateFlagsInfo mfi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
		mfi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext = &mfi;
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = memIdx(mr.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		check("AllocBufMemA", vkAllocateMemory(dev, &ai, nullptr, &m));
		check("BindBufferA", vkBindBufferMemory(dev, b, m, 0));
		if (mapOut) check("MapMemoryA", vkMapMemory(dev, m, 0, size, 0, mapOut));
	};
	auto bufAddr = [&](VkBuffer b) {
		VkBufferDeviceAddressInfo di{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
		di.buffer = b;
		return vkGetBufferDeviceAddress(dev, &di);
	};
	VkBuffer rowBuf = VK_NULL_HANDLE, rangeBuf = VK_NULL_HANDLE, clusterBuf = VK_NULL_HANDLE,
			 trisBuf = VK_NULL_HANDLE, remapBuf = VK_NULL_HANDLE, poolBuf = VK_NULL_HANDLE;
	VkDeviceMemory rowMem, rangeMem, clusterMem, trisMem, remapMem, poolMem;
	void *rowMap = nullptr;
	VkDeviceSize rowBytesTotal = 0;
	const uint32_t chainVal = o.triv ? 0u : 1u; // mesh_chain 的 CHAIN spec 常量
	VkSpecializationMapEntry csme{0, 0, 4};
	VkSpecializationInfo cspec{};
	cspec.mapEntryCount = 1; cspec.pMapEntries = &csme;
	cspec.dataSize = 4; cspec.pData = &chainVal;
	if (o.chain) {
		uint64_t s = 0x5EED26ull; // 静态表 LCG 定种子（可复现）
		auto rnd = [&]() {
			s = (s * 6364136223846793005ull + 1442695040888963407ull) >> 16;
			return (uint32_t)(s >> 17);
		};
		rangeHost.assign(NOBJ * 2, 0);
		clusterHost.assign((size_t)NOBJ * CPER * 11, 0);
		std::vector<uint8_t> trisB;
		for (uint32_t oi = 0; oi < NOBJ; ++oi) {
			rangeHost[oi * 2] = oi * CPER; rangeHost[oi * 2 + 1] = CPER;
			for (uint32_t c = 0; c < CPER; ++c) {
				uint32_t ci = oi * CPER + c, cb = ci * 11u;
				uint32_t triCount = 16u + rnd() % 113u; // 16..128（引擎 ≤128 契约）
				uint32_t vertCount = 16u + rnd() % 113u;
				clusterHost[cb + 2] = triCount; clusterHost[cb + 3] = vertCount;
				clusterHost[cb + 4] = (uint32_t)trisB.size(); // Tris 段字节偏移
				clusterHost[cb + 5] = totalVerts;             // Remap 段元素偏移
				for (uint32_t t = 0; t < triCount; ++t)
					for (uint32_t k = 0; k < 3; ++k) trisB.push_back((uint8_t)(rnd() % vertCount));
				for (uint32_t vv = 0; vv < vertCount; ++vv) remapHost.push_back(totalVerts + vv);
				totalVerts += vertCount;
			}
		}
		trisBytes = (VkDeviceSize)((trisB.size() + 16u) & ~3ull); // 尾 padding：loadU8x3 尾字恒可读
		trisHostW.assign((size_t)trisBytes / 4, 0);
		memcpy(trisHostW.data(), trisB.data(), trisB.size());
		poolHost.assign((size_t)totalVerts * 12u, 0);
		auto f01 = [&]() { return (float)(rnd() >> 8) / (float)(1u << 24); };
		for (uint32_t vv = 0; vv < totalVerts; ++vv) {
			poolHost[vv * 12u + 0] = f2u(f01() * 1.8f - 0.9f); // @0 = vec3 位置（clip XY ∈ [-0.9,0.9]）
			poolHost[vv * 12u + 1] = f2u(f01() * 1.8f - 0.9f);
			poolHost[vv * 12u + 2] = f2u(f01() * 0.9f + 0.05f); // z ∈ [0.05,0.95]（深度写进真值）
		}
		rowBytesTotal = (VkDeviceSize)kRing * rowsPerSlot * 24u;
		makeBufferAddr(rowBytesTotal,
			(VkBufferUsageFlags)(VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
			rowBuf, rowMem, &rowMap);
		for (uint32_t i = 0; i < kRing; ++i) // 每个 ring 区各填一份（rowBase 绝对行号口径）
			fillRows((char *)rowMap + (size_t)i * rowsPerSlot * 24u);
		// 静态表统一走 makeBufferAddr（下面六枚描述符要设备地址）
#define CHAIN_STATIC(name, hostVec, bytes) \
		makeBufferAddr(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, name##Buf, name##Mem, nullptr); \
		{ void *mm = nullptr; check("MapS", vkMapMemory(dev, name##Mem, 0, bytes, 0, &mm)); \
		  memcpy(mm, hostVec.data(), bytes); vkUnmapMemory(dev, name##Mem); }
		CHAIN_STATIC(range, rangeHost, (VkDeviceSize)rangeHost.size() * 4)
		CHAIN_STATIC(cluster, clusterHost, (VkDeviceSize)clusterHost.size() * 4)
		CHAIN_STATIC(tris, trisHostW, trisBytes)
		CHAIN_STATIC(remap, remapHost, (VkDeviceSize)remapHost.size() * 4)
		CHAIN_STATIC(pool, poolHost, (VkDeviceSize)poolHost.size() * 4)
#undef CHAIN_STATIC
	}

	// ---- §26 heap 档：堆缓冲 + 六枚描述符 + ESO（引擎 shadow_meshlet_mdi 条目的引擎外镜像）----
	VkBuffer heapBuf = VK_NULL_HANDLE; VkDeviceMemory heapMem; void *heapMap = nullptr;
	VkDeviceAddress heapAddr = 0;
	VkDeviceSize heapSize = 0, heapReservedOff = 0, heapReservedSize = 0, rStride = 0;
	VkShaderEXT shTask = VK_NULL_HANDLE, shMesh = VK_NULL_HANDLE, shFrag = VK_NULL_HANDLE;
	uint32_t pushWords[3] = {0, 0, 0}; // 引擎 DrawArgs {rowBase, cascade, reserved}
	if (o.chain && o.heap) {
		VkPhysicalDeviceDescriptorHeapPropertiesEXT hprops{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
		VkPhysicalDeviceProperties2 hp2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		hp2.pNext = &hprops;
		vkGetPhysicalDeviceProperties2(pd, &hp2);
		rStride = hprops.bufferDescriptorSize;
		heapReservedSize = hprops.minResourceHeapReservedRange;
		heapSize = rStride * 6u + heapReservedSize;
		heapReservedOff = heapSize - heapReservedSize;
		makeBufferAddr(heapSize, (VkBufferUsageFlags)VK_BUFFER_USAGE_2_DESCRIPTOR_HEAP_BIT_EXT,
			heapBuf, heapMem, &heapMap);
		heapAddr = bufAddr(heapBuf);
		struct { VkBuffer b; VkDeviceSize sz; } res[6] = {
			{rowBuf, rowBytesTotal}, {rangeBuf, (VkDeviceSize)rangeHost.size() * 4},
			{clusterBuf, (VkDeviceSize)clusterHost.size() * 4}, {trisBuf, trisBytes},
			{remapBuf, (VkDeviceSize)remapHost.size() * 4}, {poolBuf, (VkDeviceSize)poolHost.size() * 4}};
		VkResourceDescriptorInfoEXT rdi[6]; VkDeviceAddressRangeEXT bar[6];
		VkHostAddressRangeEXT dst[6];
		for (uint32_t i = 0; i < 6; ++i) {
			rdi[i] = {VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT, nullptr,
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, {}};
			bar[i] = {bufAddr(res[i].b), res[i].sz};
			rdi[i].data.pAddressRange = &bar[i];
			dst[i] = {(char *)heapMap + (size_t)i * (size_t)rStride, (size_t)rStride};
		}
		check("WriteResDesc", vkWriteResourceDescriptorsEXT(dev, 6, rdi, dst));
		// ESO 三件套：flags=DESCRIPTOR_HEAP（⟹ setLayoutCount=0、pushConstantRangeCount=0，
		// push 常量经 push data 供给 = VUID-11290/11370，CTS 合并测试同款踩点）
		auto makeSO = [&](VkShaderStageFlagBits stage, VkShaderStageFlagBits next,
			const char *path, const VkSpecializationInfo *spi,
			const VkShaderDescriptorSetAndBindingMappingInfoEXT *mi) {
			auto code = readSpv(path);
			VkShaderCreateInfoEXT sci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
			sci.flags = VkShaderCreateFlagsEXT(VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT);
			sci.stage = stage; sci.nextStage = next;
			sci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
			sci.codeSize = code.size() * 4; sci.pCode = code.data(); sci.pName = "main";
			sci.setLayoutCount = 0; sci.pSetLayouts = nullptr;
			sci.pushConstantRangeCount = 0; sci.pPushConstantRanges = nullptr;
			sci.pSpecializationInfo = spi;
			sci.pNext = mi;
			VkShaderEXT sh;
			check("CreateShadersEXT", vkCreateShadersEXT(dev, 1u, &sci, nullptr, &sh));
			return sh;
		};
		// 绑定映射：task binding0←堆槽0；mesh binding1..5←槽1..5（逐枚 constant offset，引擎烘法）
		VkDescriptorSetAndBindingMappingEXT mapTask[1], mapMesh[5];
		for (auto &m : mapTask) {
			m = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT, nullptr, 0u, 0u, 1u,
				VK_SPIRV_RESOURCE_TYPE_ALL_EXT,
				VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT, {}};
			mapTask[0].sourceData.constantOffset = {0u, 0u};
		}
		for (uint32_t i = 0; i < 5; ++i) {
			mapMesh[i] = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT, nullptr, 0u,
				1u + i, 1u, VK_SPIRV_RESOURCE_TYPE_ALL_EXT,
				VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT, {}};
			mapMesh[i].sourceData.constantOffset = {(uint32_t)((i + 1) * (uint64_t)rStride), 0u};
		}
		VkShaderDescriptorSetAndBindingMappingInfoEXT miTask{
			VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT, nullptr, 1u, mapTask};
		VkShaderDescriptorSetAndBindingMappingInfoEXT miMesh{
			VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT, nullptr, 5u, mapMesh};
		shTask = makeSO(VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT,
			mdi ? "shaders/task_engine.spv" : "shaders/task_engine_dd.spv", nullptr, &miTask);
		shMesh = makeSO(VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT,
			"shaders/mesh_chain.spv", &cspec, &miMesh);
		shFrag = makeSO(VK_SHADER_STAGE_FRAGMENT_BIT, VkShaderStageFlagBits(0u),
			"shaders/frag.spv", nullptr, nullptr);
	}

	// ---- §26 集档（R3）：六 binding 描述符集 + 经典管线（heap 是否必要的对照组）----
	VkPipeline pipeChain = VK_NULL_HANDLE;
	VkPipelineLayout plChain = VK_NULL_HANDLE;
	VkDescriptorSet dsetChain = VK_NULL_HANDLE;
	if (o.chain && !o.heap) {
		VkDescriptorSetLayoutBinding cdb[6] = {
			{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_TASK_BIT_EXT, nullptr},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
			{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
			{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
			{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr},
			{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_MESH_BIT_EXT, nullptr}};
		VkDescriptorSetLayoutCreateInfo cdli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		cdli.bindingCount = 6; cdli.pBindings = cdb;
		VkDescriptorSetLayout dslChain;
		check("CreateDSLc", vkCreateDescriptorSetLayout(dev, &cdli, nullptr, &dslChain));
		VkDescriptorPoolSize cps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6};
		VkDescriptorPoolCreateInfo cpi2{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		cpi2.maxSets = 1; cpi2.poolSizeCount = 1; cpi2.pPoolSizes = &cps;
		VkDescriptorPool dpChain;
		check("CreateDPc", vkCreateDescriptorPool(dev, &cpi2, nullptr, &dpChain));
		VkDescriptorSetAllocateInfo cdai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		cdai.descriptorPool = dpChain; cdai.descriptorSetCount = 1; cdai.pSetLayouts = &dslChain;
		check("AllocDSc", vkAllocateDescriptorSets(dev, &cdai, &dsetChain));
		struct { VkBuffer b; VkDeviceSize sz; } res[6] = {
			{rowBuf, rowBytesTotal}, {rangeBuf, (VkDeviceSize)rangeHost.size() * 4},
			{clusterBuf, (VkDeviceSize)clusterHost.size() * 4}, {trisBuf, trisBytes},
			{remapBuf, (VkDeviceSize)remapHost.size() * 4}, {poolBuf, (VkDeviceSize)poolHost.size() * 4}};
		VkDescriptorBufferInfo dbi[6]; VkWriteDescriptorSet wd[6];
		for (uint32_t i = 0; i < 6; ++i) {
			dbi[i] = {res[i].b, 0, res[i].sz};
			wd[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dsetChain, i, 0, 1,
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dbi[i], nullptr};
		}
		vkUpdateDescriptorSets(dev, 6, wd, 0, nullptr);
		VkPushConstantRange pcr{VK_SHADER_STAGE_TASK_BIT_EXT, 0u, 12u};
		VkPipelineLayoutCreateInfo cpli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		cpli.setLayoutCount = 1; cpli.pSetLayouts = &dslChain;
		cpli.pushConstantRangeCount = 1; cpli.pPushConstantRanges = &pcr;
		check("CreatePLc", vkCreatePipelineLayout(dev, &cpli, nullptr, &plChain));
		std::array<VkPipelineShaderStageCreateInfo, 3> cstg{};
		cstg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cstg[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
		cstg[0].module = loadMod(mdi ? "shaders/task_engine.spv" : "shaders/task_engine_dd.spv");
		cstg[0].pName = "main";
		cstg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cstg[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
		cstg[1].module = loadMod("shaders/mesh_chain.spv");
		cstg[1].pName = "main"; cstg[1].pSpecializationInfo = &cspec;
		cstg[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cstg[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT; cstg[2].module = fragM; cstg[2].pName = "main";
		VkGraphicsPipelineCreateInfo cgpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		cgpi.stageCount = 3; cgpi.pStages = cstg.data();
		cgpi.pVertexInputState = &vin; cgpi.pInputAssemblyState = &ia; cgpi.pViewportState = &vp;
		cgpi.pRasterizationState = &rs; cgpi.pMultisampleState = &ms; cgpi.pDepthStencilState = &ds;
		cgpi.pColorBlendState = &cb; cgpi.pDynamicState = &dsi;
		cgpi.layout = plChain; cgpi.renderPass = rp; cgpi.subpass = 0;
		check("CreatePipec", vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &cgpi, nullptr, &pipeChain));
	}

	// ---- 描述符（buffer 模式，ring×4 一套一槽）----
	const bool needSets = true; // mesh.glsl 恒声明 rows_/xsrc（死代码也算静态使用）→ set 恒备
	VkDescriptorPool dp = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, kRing> dsets{};
	if (needSets) {
		VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kRing * 2};
		VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpi.maxSets = kRing; dpi.poolSizeCount = 1; dpi.pPoolSizes = &dps;
		check("CreateDP", vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
		VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dsai.descriptorPool = dp; dsai.descriptorSetCount = kRing; dsai.pSetLayouts = dsls.data();
		check("AllocDS", vkAllocateDescriptorSets(dev, &dsai, dsets.data()));
	}

	// ---- 数据缓冲（ring×4：引擎 ring×2 的近似，帧间不竞态）----
	const VkDeviceSize indSize = (VkDeviceSize)rows * o.stride;
	const VkDeviceSize xSize = (VkDeviceSize)(tasksPerDispatch + 1) * 4;
	std::vector<uint32_t> indHost(rows * (o.stride / 4), 0);
	std::vector<uint32_t> xHost(tasksPerDispatch + 1, o.x);
	for (uint32_t r = 0; r < rows; ++r) {
		indHost[r * (o.stride / 4) + 0] = o.tasks; // {taskX, 1, 1}（+ 引擎同款 24B 填充）
		indHost[r * (o.stride / 4) + 1] = 1;
		indHost[r * (o.stride / 4) + 2] = 1;
	}
	if (o.vary) { // 引擎形状：写序 LCG 固定种子（可复现）
		uint64_t s = 0x5DEECE66Dull;
		auto rnd = [&]() { s = (s * 6364136223846793005ull + 1442695040888963407ull) >> 16; return (uint32_t)(s >> 17); };
		for (uint32_t r = 0; r < rows; ++r) {
			if (o.notask)
				indHost[r * (o.stride / 4) + 0] = 2u + rnd() % 17u;          // 真 M-1b：行 X = mesh 组数 2..18
			else
				indHost[r * (o.stride / 4) + 0] = (rnd() & 1u) ? o.tasks : 0u; // 孔洞流 gcX∈{0,1}
			xHost[r] = 2u + rnd() % 17u;                                     // 2..18 可变组数
		}
	}
	std::array<VkBuffer, kRing> indBufs{}, xBufs{};
	std::array<VkDeviceMemory, kRing> indMems{}, xMems{};
	std::array<void *, kRing> indMaps{}, xMaps{};
	for (uint32_t i = 0; i < kRing; ++i) {
		if (mdi) {
			makeBuffer(indSize, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, indBufs[i], indMems[i], &indMaps[i]);
			memcpy(indMaps[i], indHost.data(), indSize);
		}
		if (needSets) {
			makeBuffer(xSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, xBufs[i], xMems[i], &xMaps[i]);
			memcpy(xMaps[i], xHost.data(), xSize);
			VkDescriptorBufferInfo dbi{xBufs[i], 0, xSize};
			std::array<VkWriteDescriptorSet, 2> w{};
			for (uint32_t b = 0; b < 2; ++b) {
				w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				w[b].dstSet = dsets[i];
				w[b].dstBinding = b;
				w[b].descriptorCount = 1;
				w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
				w[b].pBufferInfo = &dbi;
			}
			vkUpdateDescriptorSets(dev, 2, w.data(), 0, nullptr);
		}
	}

	// ---- 帧循环（async = 帧间不等 fence，ring×4 自然限流；sync = 每帧等）----
	VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = qfi;
	VkCommandPool cp;
	check("CreateCP", vkCreateCommandPool(dev, &cpi, nullptr, &cp));
	VkCommandBufferAllocateInfo caib{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	caib.commandPool = cp; caib.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; caib.commandBufferCount = kRing;
	std::array<VkCommandBuffer, kRing> cmds;
	vkAllocateCommandBuffers(dev, &caib, cmds.data());
	caib.commandBufferCount = 1; // 后续单发分配（回读用）
	std::array<VkFence, kRing> fences;
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
	for (auto &fn : fences) check("CreateFence", vkCreateFence(dev, &fci, nullptr, &fn));
	VkFence fence = fences[0]; // 回读用
	std::array<VkClearValue, 2> clr{};
	clr[0].color = {0, 0, 0, 1};
	clr[1].depthStencil = {1.0f, 0};
	VkViewport vpt{0, 0, (float)W, (float)H, 0, 1};
	VkRect2D sci{{0, 0}, {W, H}};

	for (uint32_t f = 0; f < o.frames; ++f) {
		const uint32_t slot = f % kRing;
		vkWaitForFences(dev, 1, &fences[slot], VK_TRUE, ~0ull); // ring 内自然限流（async = 4 帧 in-flight；sync = 1 帧）
		if (persist) { // 引擎形状 = 持久映射每帧改写（HOST_COHERENT，无需 flush）
			if (mdi) memcpy(indMaps[slot], indHost.data(), indSize);
			if (bufMode) memcpy(xMaps[slot], xHost.data(), xSize);
		}
		vkResetFences(dev, 1, &fences[slot]);
		VkCommandBuffer cmd = cmds[slot];
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		check("BeginCmd", vkBeginCommandBuffer(cmd, &bi));
		if (o.chain) {
			// ---- §26 链档帧体（CB 每帧重录 = 引擎同形；行表持久映射每帧可改写）----
			if (o.rowvary) // 引擎口径 = 每帧只重写本帧槽（LCG 续流 = 逐帧剔除变化）
				fillRows((char *)rowMap + (size_t)slot * rowsPerSlot * 24u);
			if (o.heap) {
				if (f == 0u) { // 一次性 layout 转换（dynamic rendering 不自动转，09588/09592 实测）
					VkImageMemoryBarrier lb[2]{};
					lb[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0,
						VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
						VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
						VK_QUEUE_FAMILY_IGNORED, colorImg, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
					lb[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0,
						VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
						VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
						VK_QUEUE_FAMILY_IGNORED, depthImg, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
					vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
						VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
							| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
						0, 0, nullptr, 0, nullptr, 2, lb);
				}
				VkRenderingAttachmentInfo caiT{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
				caiT.imageView = colorVw; caiT.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				caiT.resolveMode = VK_RESOLVE_MODE_NONE;
				caiT.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; caiT.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				caiT.clearValue.color = {{0, 0, 0, 1}};
				VkRenderingAttachmentInfo daiT{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
				daiT.imageView = depthVw; daiT.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				daiT.resolveMode = VK_RESOLVE_MODE_NONE;
				daiT.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; daiT.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
				daiT.clearValue.depthStencil = {1.0f, 0};
				VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
				ri.renderArea = {{0, 0}, {W, H}}; ri.layerCount = 1;
				ri.colorAttachmentCount = 1; ri.pColorAttachments = &caiT; ri.pDepthAttachment = &daiT;
				vkCmdBeginRendering(cmd, &ri);
				VkBindHeapInfoEXT bh{VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
				bh.heapRange = {heapAddr, heapSize};
				bh.reservedRangeOffset = heapReservedOff; bh.reservedRangeSize = heapReservedSize;
				vkCmdBindResourceHeapEXT(cmd, &bh);
				const VkShaderStageFlagBits st3[3] = {VK_SHADER_STAGE_TASK_BIT_EXT,
					VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT};
				const VkShaderEXT sh3[3] = {shTask, shMesh, shFrag};
				vkCmdBindShadersEXT(cmd, 3, st3, sh3);
				const VkShaderStageFlagBits nullSt[4] = {VK_SHADER_STAGE_VERTEX_BIT,
					VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
					VK_SHADER_STAGE_GEOMETRY_BIT};
				const VkShaderEXT nullSh[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
				vkCmdBindShadersEXT(cmd, 4, nullSt, nullSh);
				// ESO 基线动态状态（全动态 = ESO 语义；深度测试+写开 = 引擎 shadow 段同形）
				{
					const VkViewport v = {0, 0, (float)W, (float)H, 0, 1};
					const VkRect2D r = {{0, 0}, {W, H}};
					const float bc[4] = {0, 0, 0, 0};
					const VkBool32 bFalse = VK_FALSE;
					const VkColorComponentFlags wmask = 0xF;
					const VkColorBlendEquationEXT eq = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO,
						VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
					const VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
					const VkSampleMask sampleMask = 0xFFFFFFFFu;
					vkCmdSetViewportWithCount(cmd, 1, &v);
					vkCmdSetScissorWithCount(cmd, 1, &r);
					vkCmdSetLineWidth(cmd, 1.0f);
					vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
					vkCmdSetBlendConstants(cmd, bc);
					vkCmdSetDepthBounds(cmd, 0.0f, 1.0f);
					vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFFFFFFFFu);
					vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFFFFFFFFu);
					vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0u);
					vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
					vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
					vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
					vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
					vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
					vkCmdSetDepthTestEnable(cmd, VK_TRUE);
					vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
					vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_LESS);
					vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
					vkCmdSetStencilTestEnable(cmd, VK_FALSE);
					vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_OP_KEEP,
						VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
					vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, VK_STENCIL_OP_KEEP,
						VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
					vkCmdSetLogicOpEnableEXT(cmd, VK_FALSE);
					vkCmdSetLogicOpEXT(cmd, VK_LOGIC_OP_COPY);
					vkCmdSetColorBlendEnableEXT(cmd, 0u, 1u, &bFalse);
					vkCmdSetColorBlendEquationEXT(cmd, 0u, 1u, &eq);
					vkCmdSetColorWriteMaskEXT(cmd, 0u, 1u, &wmask);
					vkCmdSetAlphaToCoverageEnableEXT(cmd, VK_FALSE);
					vkCmdSetAlphaToOneEnableEXT(cmd, VK_FALSE);
					vkCmdSetVertexInputEXT(cmd, 0u, nullptr, 0u, nullptr);
					vkCmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_FILL);
					vkCmdSetRasterizationSamplesEXT(cmd, samples);
					vkCmdSetSampleMaskEXT(cmd, samples, &sampleMask);
					vkCmdSetDepthClampEnableEXT(cmd, VK_FALSE);
				}
				for (uint32_t d = 0; d < segs; ++d) {
					const uint32_t rb = slot * rowsPerSlot + d * rows;
					pushWords[0] = rb; pushWords[1] = d; pushWords[2] = 0;
					VkPushDataInfoEXT pdi{VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT};
					pdi.offset = 0; pdi.data = {(const void *)pushWords, sizeof(pushWords)};
					vkCmdPushDataEXT(cmd, &pdi); // 记录期快照（§24 工程副产口径）
					if (mdi) vkCmdDrawMeshTasksIndirectEXT(cmd, rowBuf, (VkDeviceSize)rb * 24u, rows, 24);
					else vkCmdDrawMeshTasksEXT(cmd, rows, 1, 1); // R4 = 引擎 S3 镜像
				}
				vkCmdEndRendering(cmd);
			} else {
				VkRenderPassBeginInfo rbiC{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
				rbiC.renderPass = rp; rbiC.framebuffer = fb;
				rbiC.renderArea = {{0, 0}, {W, H}};
				rbiC.clearValueCount = 2; rbiC.pClearValues = clr.data();
				vkCmdBeginRenderPass(cmd, &rbiC, VK_SUBPASS_CONTENTS_INLINE);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeChain);
				vkCmdSetViewport(cmd, 0, 1, &vpt);
				vkCmdSetScissor(cmd, 0, 1, &sci);
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, plChain, 0, 1, &dsetChain, 0, nullptr);
				for (uint32_t d = 0; d < segs; ++d) {
					const uint32_t rb = slot * rowsPerSlot + d * rows;
					pushWords[0] = rb; pushWords[1] = d; pushWords[2] = 0;
					vkCmdPushConstants(cmd, plChain, VK_SHADER_STAGE_TASK_BIT_EXT, 0, 12, pushWords);
					if (mdi) vkCmdDrawMeshTasksIndirectEXT(cmd, rowBuf, (VkDeviceSize)rb * 24u, rows, 24);
					else vkCmdDrawMeshTasksEXT(cmd, rows, 1, 1);
				}
				vkCmdEndRenderPass(cmd);
			}
			check("EndCmd", vkEndCommandBuffer(cmd));
			VkSubmitInfo siC{VK_STRUCTURE_TYPE_SUBMIT_INFO};
			siC.commandBufferCount = 1; siC.pCommandBuffers = &cmd;
			check("Submit", vkQueueSubmit(queue, 1, &siC, fences[slot]));
			if (!o.async) vkWaitForFences(dev, 1, &fences[slot], VK_TRUE, ~0ull);
			continue;
		}
		VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rbi.renderPass = rp; rbi.framebuffer = fb;
		rbi.renderArea = {{0, 0}, {W, H}};
		rbi.clearValueCount = 2; rbi.pClearValues = clr.data();
		vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		vkCmdSetViewport(cmd, 0, 1, &vpt);
		vkCmdSetScissor(cmd, 0, 1, &sci);
		if (needSets) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dsets[slot], 0, nullptr);
		for (uint32_t d = 0; d < o.dispatches; ++d) {
			if (o.rebind && d > 0) { // 引擎形状：派发之间重绑（DI 基址清零重设）
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				vkCmdSetViewport(cmd, 0, 1, &vpt);
				vkCmdSetScissor(cmd, 0, 1, &sci);
				if (needSets) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dsets[slot], 0, nullptr);
			}
			if (mdi) vkCmdDrawMeshTasksIndirectEXT(cmd, indBufs[slot], 0, rows, o.stride);
			else vkCmdDrawMeshTasksEXT(cmd, o.tasks, 1, 1);
			if (o.intermesh && !o.twopass) { // 候选②档①：MDI 之间插大型 mesh 直派
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeM);
				vkCmdDrawMeshTasksEXT(cmd, o.intermesh, 1, 1);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				if (needSets) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dsets[slot], 0, nullptr);
			}
			if (o.intertask && !o.twopass) { // 候选②档②：直派臂 = 主管线 task 驱动（数据驱动组数）
				vkCmdDrawMeshTasksEXT(cmd, o.intertask, 1, 1);
			}
			if (o.interleave) { // MDI 之间插经典 draw（队列里有别的活）
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeV);
				vkCmdDraw(cmd, 3, 1, 0, 0);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				if (needSets) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dsets[slot], 0, nullptr);
			}
		}
		vkCmdEndRenderPass(cmd);
		if (o.twopass) { // 候选②档③：第二 pass（同 color+depth 靶）= attachment 切换
			VkRenderPassBeginInfo rb2{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			rb2.renderPass = rp2; rb2.framebuffer = fb2;
			rb2.renderArea = {{0, 0}, {W, H}};
			vkCmdBeginRenderPass(cmd, &rb2, VK_SUBPASS_CONTENTS_INLINE);
			if (o.intermesh) {
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeM);
				vkCmdDrawMeshTasksEXT(cmd, o.intermesh * o.dispatches, 1, 1);
			}
			if (o.intertask) {
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
				if (needSets) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dsets[slot], 0, nullptr);
				vkCmdDrawMeshTasksEXT(cmd, o.intertask * o.dispatches, 1, 1);
			}
			vkCmdEndRenderPass(cmd);
		}
		check("EndCmd", vkEndCommandBuffer(cmd));
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
		check("Submit", vkQueueSubmit(queue, 1, &si, fences[slot]));
		if (!o.async) vkWaitForFences(dev, 1, &fences[slot], VK_TRUE, ~0ull); // 同步档 = 每帧落定
	}
	for (auto &fn : fences) {
		VkResult wr = vkWaitForFences(dev, 1, &fn, VK_TRUE, 120ull * 1000000000ull);
		if (wr == VK_ERROR_DEVICE_LOST) { std::fprintf(stderr, "DEVICE_LOST during wait\n"); return 4; }
		check("WaitFences", wr);
	}

	// ---- 回读校验和 ----
	VkBuffer rbBuf; VkDeviceMemory rbMem; void *rbMap = nullptr;
	makeBuffer((VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rbBuf, rbMem, &rbMap);
	VkCommandBuffer cmd2;
	check("AllocCmd2", vkAllocateCommandBuffers(dev, &caib, &cmd2));
	VkCommandBufferBeginInfo bi2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	check("BeginCmd2", vkBeginCommandBuffer(cmd2, &bi2));
	VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	ib.image = colorImg;
	ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd2, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
	VkBufferImageCopy cp_{};
	cp_.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cp_.imageSubresource.layerCount = 1;
	cp_.imageExtent = {W, H, 1};
	vkCmdCopyImageToBuffer(cmd2, colorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbBuf, 1, &cp_);
	check("EndCmd2", vkEndCommandBuffer(cmd2));
	check("ResetFence", vkResetFences(dev, 1, &fence));
	VkSubmitInfo si2{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si2.commandBufferCount = 1; si2.pCommandBuffers = &cmd2;
	check("Submit2", vkQueueSubmit(queue, 1, &si2, fence));
	check("WaitReadback", vkWaitForFences(dev, 1, &fence, VK_TRUE, 60ull * 1000000000ull));
	uint64_t sum = 0, lit = 0;
	auto *px = (const uint8_t *)rbMap;
	for (uint32_t i = 0; i < W * H; ++i) { sum += px[i * 4]; if (px[i * 4] > 127) ++lit; }
	printf("{\"tasks\":%u,\"x\":%u,\"rows\":%u,\"dispatches\":%u,\"frames\":%u,\"mode\":\"%s\",\"xsrc\":\"%s\",\"reuse\":\"%s\",\"async\":%s,\"interleave\":%s,\"heavy\":%s,\"vary\":%s,\"meshdrawid\":%s,\"meshread\":%s,\"intermesh\":%u,\"intertask\":%u,\"twopass\":%s,\"chain\":%u,\"triv\":%u,\"heap\":%s,\"rowvary\":%s,\"total_groups\":%llu,\"checksum\":%llu,\"lit_pixels\":%llu,\"device\":\"%s\",\"driver\":%u}\n",
		o.tasks, o.x, rows, o.dispatches, o.frames, o.mode.c_str(), o.xsrc.c_str(), o.reuse.c_str(),
		o.async ? "true" : "false", o.interleave ? "true" : "false", o.heavy ? "true" : "false",
		o.vary ? "true" : "false", o.meshdrawid ? "true" : "false",
		o.meshread ? "true" : "false",
		o.intermesh, o.intertask, o.twopass ? "true" : "false",
		o.chain, o.triv, o.heap ? "true" : "false", o.rowvary ? "true" : "false",
		(unsigned long long)totalGroups, (unsigned long long)sum, (unsigned long long)lit,
		props.deviceName, props.driverVersion);
	vkDeviceWaitIdle(dev);
	return 0;
}
