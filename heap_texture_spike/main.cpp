// heap_texture_spike — Phase 3 预研：descriptor heap 中存 SAMPLED_IMAGE 描述符 +
// 内嵌 sampler（pEmbeddedSampler）+ shader object 采样纹理，离屏渲染 + 像素回读验证。
//
// 回答的问题（spec 调研见 docs/heap_texture_research.md）：
//   Q1. vkGetPhysicalDeviceDescriptorSizeEXT(SAMPLED_IMAGE) vs bufferDescriptorSize
//       —— 现有 slot 寻址（slot * bufferDescriptorSize）能否直接容纳 image 描述符？
//   Q2. imageDescriptorAlignment vs bufferDescriptorAlignment / descriptorSize
//       —— 统一 slot 步长是否为 image binding 的合法 offset 倍数（image 偏移对齐 VUID）？
//   Q3. 同一个 resource heap 混存 buffer + image 描述符是否可行？
//   Q4. pEmbeddedSampler 在 mesh+frag shader object 上是否工作（两驱动）？
//
// 无窗口、无 SDL：离屏 256x256 渲染 + copy 回读 + CPU 像素比对（PASS/FAIL）。
// 驱动由 VK_DRIVER_FILES 环境变量决定（RADV: RADV_EXPERIMENTAL=heap；NVIDIA: prime offload）。
// API 按系统 Vulkan 头 v341（与主工程同一编译环境）：buffer 描述符走 pAddressRange 裸地址。

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <algorithm>

#define CHK(r) do{VkResult _r=(r);if(_r!=VK_SUCCESS){fprintf(stderr,"FAIL line %d: %s -> %d\n",__LINE__,#r,(int)_r);return 1;}}while(0)
#define CHK2(r) do{VkResult _r=(r);if(_r!=VK_SUCCESS){fprintf(stderr,"FAIL line %d: %s -> %d\n",__LINE__,#r,(int)_r);g_failed=true;}}while(0)

static bool g_failed = false;

static VKAPI_ATTR VkBool32 VKAPI_CALL dbgCb(
	VkDebugUtilsMessageSeverityFlagBitsEXT sev,
	VkDebugUtilsMessageTypeFlagsEXT,
	const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
	if (sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
		fprintf(stderr, "[VVL %s%d] %s\n", d->pMessageIdName ? d->pMessageIdName : "", d->messageIdNumber, d->pMessage);
	return VK_FALSE;
}

static std::vector<uint8_t> readBin(const char* p) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	auto e = f.tellg();
	if (e <= 0) { fprintf(stderr, "cannot read %s\n", p); return {}; }
	f.seekg(0);
	std::vector<uint8_t> b((size_t)e);
	f.read((char*)b.data(), e);
	return b;
}

static bool pixelClose(const uint8_t* px, float r, float g, float b, float a, int tol = 2) {
	auto close1 = [](uint8_t v, float f, int tol) {
		float ef = f * 255.0f;
		if (ef > 255.0f) ef = 255.0f;
		return std::abs((float)v - ef) <= (float)tol;
	};
	return close1(px[0], r, tol) && close1(px[1], g, tol) && close1(px[2], b, tol) && close1(px[3], a, tol);
}

using ShaderHandle = void*;

int main() {
	const uint32_t W = 256, H = 256;
	const float bufVal = 0.1f; // 存储缓冲值，fragment 加到采样结果上

	// --- 1. instance ---
	bool haveValidation = false;
	{
		uint32_t lc = 0;
		vkEnumerateInstanceLayerProperties(&lc, nullptr);
		std::vector<VkLayerProperties> layers(lc);
		if (lc) vkEnumerateInstanceLayerProperties(&lc, layers.data());
		for (auto& l : layers)
			if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) { haveValidation = true; break; }
	}
	fprintf(stderr, "validation layer: %s\n", haveValidation ? "available" : "NOT available");

	VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ai.pApplicationName = "heap_texture_spike";
	ai.apiVersion = VK_API_VERSION_1_3;

	VkDebugUtilsMessengerCreateInfoEXT dmci = {};
	if (haveValidation) {
		dmci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		dmci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		dmci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
		dmci.pfnUserCallback = dbgCb;
	}
	VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ic.pApplicationInfo = &ai;
	if (haveValidation) {
		ic.pNext = &dmci;
		const char* layer = "VK_LAYER_KHRONOS_validation";
		const char* iext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME; // messenger 要求实例扩展
		ic.enabledLayerCount = 1;
		ic.ppEnabledLayerNames = &layer;
		ic.enabledExtensionCount = 1;
		ic.ppEnabledExtensionNames = &iext;
	}
	VkInstance inst;
	CHK(vkCreateInstance(&ic, nullptr, &inst));

	// --- 2. 设备选择（VK_DRIVER_FILES 决定驱动，选第一个 graphics 队列）---
	uint32_t pc = 0;
	vkEnumeratePhysicalDevices(inst, &pc, nullptr);
	std::vector<VkPhysicalDevice> pds(pc);
	vkEnumeratePhysicalDevices(inst, &pc, pds.data());
	VkPhysicalDevice pd = nullptr;
	uint32_t gq = 0;
	for (uint32_t i = 0; i < pc; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pds[i], &p);
		fprintf(stderr, "  GPU%d: %s (vendor=0x%x)\n", i, p.deviceName, p.vendorID);
		uint32_t qfc = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qfc, nullptr);
		std::vector<VkQueueFamilyProperties> qfs(qfc);
		vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qfc, qfs.data());
		for (uint32_t q = 0; q < qfc; q++)
			if (qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { pd = pds[i]; gq = q; break; }
		if (pd) break;
	}
	if (!pd) { fprintf(stderr, "no graphics device\n"); return 1; }
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(pd, &pp);
	fprintf(stderr, "Selected: %s (apiVersion %u.%u.%u)\n", pp.deviceName,
		VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion));

	// --- 3. 关键查询（Q1/Q2 的答案都在这里）---
	VkPhysicalDeviceDescriptorHeapPropertiesEXT hp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
	VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p2.pNext = &hp;
	vkGetPhysicalDeviceProperties2(pd, &p2);

	fprintf(stderr, "\n=== heap properties ===\n");
	fprintf(stderr, "bufferDescriptorSize       = %zu\n", hp.bufferDescriptorSize);
	fprintf(stderr, "imageDescriptorSize        = %zu\n", hp.imageDescriptorSize);
	fprintf(stderr, "samplerDescriptorSize      = %zu\n", hp.samplerDescriptorSize);
	fprintf(stderr, "bufferDescriptorAlignment  = %zu\n", hp.bufferDescriptorAlignment);
	fprintf(stderr, "imageDescriptorAlignment   = %zu\n", hp.imageDescriptorAlignment);
	fprintf(stderr, "samplerDescriptorAlignment = %zu\n", hp.samplerDescriptorAlignment);
	fprintf(stderr, "minResourceHeapReservedRange = %zu bytes\n", hp.minResourceHeapReservedRange);
	fprintf(stderr, "maxResourceHeapSize        = %zu bytes\n", hp.maxResourceHeapSize);
	fprintf(stderr, "resourceHeapAlignment      = %zu\n", hp.resourceHeapAlignment);
	fprintf(stderr, "maxDescriptorHeapEmbeddedSamplers = %u\n", hp.maxDescriptorHeapEmbeddedSamplers);

	// Q1/Q2 预判：现有 slot 步长（bufferDescriptorSize）能否容纳 image 描述符
	uint64_t slotSize = std::max(hp.bufferDescriptorSize, hp.imageDescriptorSize);
	bool q1ok = hp.imageDescriptorSize <= hp.bufferDescriptorSize;
	bool q2ok = (slotSize % hp.imageDescriptorAlignment == 0) && (slotSize % hp.bufferDescriptorAlignment == 0);
	fprintf(stderr, "\nQ1 (imageDescSize <= bufferDescSize): %s\n", q1ok ? "PASS" : "FAIL");
	fprintf(stderr, "Q2 (slotSize %zu is multiple of imageDescAlign %zu && bufferDescAlign %zu): %s\n",
		slotSize, hp.imageDescriptorAlignment, hp.bufferDescriptorAlignment, q2ok ? "PASS" : "FAIL");
	if (!q1ok || !q2ok)
		fprintf(stderr, "  => 现有 buffer-based slot 寻址不能直接用于 image 描述符，Phase 3 需调整 slot 布局\n");

	// --- 4. device ---
	const char* de[] = {
		VK_EXT_MESH_SHADER_EXTENSION_NAME,
		VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
		VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
		VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, // 隐式依赖（与主工程一致）
		VK_KHR_MAINTENANCE_4_EXTENSION_NAME,   // descriptor heap 隐式依赖 maintenance5（VVL 01387），与主工程一致
		VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
	};
	VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	f12.bufferDeviceAddress = VK_TRUE; // BDA：描述符 pAddressRange + heap GPU 地址（与主工程一致）
	VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	f13.synchronization2 = VK_TRUE;
	f13.dynamicRendering = VK_TRUE;
	f13.maintenance4 = VK_TRUE; // LocalSizeId（glslang 对 local_size 的编码）；maintenance4 已并入 1.3 结构体，不可另挂（VUID 06532）
	VkPhysicalDeviceMeshShaderFeaturesEXT mf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
	mf.meshShader = VK_TRUE;
	VkPhysicalDeviceDescriptorHeapFeaturesEXT hf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
	hf.descriptorHeap = VK_TRUE;
	VkPhysicalDeviceShaderObjectFeaturesEXT sof = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
	sof.shaderObject = VK_TRUE;
	f12.pNext = &f13; f13.pNext = &mf; mf.pNext = &hf; hf.pNext = &sof;

	float pq = 1.0f;
	VkDeviceQueueCreateInfo dq = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	dq.queueFamilyIndex = gq; dq.queueCount = 1; dq.pQueuePriorities = &pq;
	VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dc.pNext = &f12; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &dq;
	dc.enabledExtensionCount = sizeof(de) / sizeof(*de); dc.ppEnabledExtensionNames = de;
	VkDevice dev;
	CHK(vkCreateDevice(pd, &dc, nullptr, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, gq, 0, &q);

	// 系统 loader 旧于头文件：扩展函数不在动态符号表，须运行时解析（主工程经 Vulkan-Hpp dispatcher 同理）
	#define GPA(name) (PFN_##name)vkGetDeviceProcAddr(dev, #name)
	auto writeDescFn = GPA(vkWriteResourceDescriptorsEXT);
	auto bindHeapFn = GPA(vkCmdBindResourceHeapEXT);
	// physical-device 级函数：v341 头/loader 无 vkGetPhysicalDeviceProcAddr，GPA 可能解析不到
	// （spec 标注该函数 "not intended for general use"）；best-effort，属性值（上方）才是 Q1 依据
	auto descSizeFn = GPA(vkGetPhysicalDeviceDescriptorSizeEXT);
	if (!writeDescFn || !bindHeapFn) {
		fprintf(stderr, "FAIL: extension function resolution (writeDesc=%p bindHeap=%p)\n",
			(void*)writeDescFn, (void*)bindHeapFn);
		return 1;
	}
	if (descSizeFn) {
		fprintf(stderr, "\n=== vkGetPhysicalDeviceDescriptorSizeEXT ===\n");
		fprintf(stderr, "STORAGE_BUFFER  = %zu\n", descSizeFn(pd, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
		fprintf(stderr, "UNIFORM_BUFFER  = %zu\n", descSizeFn(pd, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
		fprintf(stderr, "SAMPLED_IMAGE   = %zu\n", descSizeFn(pd, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
		fprintf(stderr, "SAMPLER         = %zu\n", descSizeFn(pd, VK_DESCRIPTOR_TYPE_SAMPLER));
	} else {
		fprintf(stderr, "\n=== vkGetPhysicalDeviceDescriptorSizeEXT: 旧 loader 无法解析（best-effort 跳过）===\n");
	}

	// --- 5. 纹理：256x256 RGBA8，左半 (0.25,0.5,0.75,1) 右半 (0.9,0.1,0.25,1) ---
	VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageCreateInfo tci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	tci.imageType = VK_IMAGE_TYPE_2D; tci.format = fmt;
	tci.extent = {W, H, 1}; tci.mipLevels = 1; tci.arrayLayers = 1;
	tci.samples = VK_SAMPLE_COUNT_1_BIT; tci.tiling = VK_IMAGE_TILING_OPTIMAL;
	tci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	tci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage texImg;
	CHK(vkCreateImage(dev, &tci, nullptr, &texImg));
	VkDeviceMemory texMem;
	{
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, texImg, &req);
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		uint32_t mt = 0;
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
		CHK(vkAllocateMemory(dev, &mai, nullptr, &texMem));
		CHK(vkBindImageMemory(dev, texImg, texMem, 0));
	}
	// image view create info 必须保留 —— VkImageDescriptorInfoEXT::pView 要的是 create info，不是 handle
	VkImageViewCreateInfo tvi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	tvi.image = texImg; tvi.viewType = VK_IMAGE_VIEW_TYPE_2D; tvi.format = fmt;
	tvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView texView;
	CHK(vkCreateImageView(dev, &tvi, nullptr, &texView));

	// 线性 staging image（host-visible）填两色，copy 到纹理
	VkImageCreateInfo lci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	lci.imageType = VK_IMAGE_TYPE_2D; lci.format = fmt;
	lci.extent = {W, H, 1}; lci.mipLevels = 1; lci.arrayLayers = 1;
	lci.samples = VK_SAMPLE_COUNT_1_BIT; lci.tiling = VK_IMAGE_TILING_LINEAR;
	lci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	lci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // copy 前转 GENERAL（VUID 01917）
	VkImage linImg;
	CHK(vkCreateImage(dev, &lci, nullptr, &linImg));
	VkDeviceMemory linMem;
	{
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, linImg, &req);
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		uint32_t mt = 0;
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) { mt = i; break; }
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
		CHK(vkAllocateMemory(dev, &mai, nullptr, &linMem));
		CHK(vkBindImageMemory(dev, linImg, linMem, 0));
		void* linMapped = nullptr;
		VkSubresourceLayout sl = {};
		VkImageSubresource sub = {};
		sub.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		sub.mipLevel = 0;
		sub.arrayLayer = 0;
		vkGetImageSubresourceLayout(dev, linImg, &sub, &sl);
		CHK(vkMapMemory(dev, linMem, 0, VK_WHOLE_SIZE, 0, &linMapped));
		for (uint32_t y = 0; y < H; y++)
			for (uint32_t x = 0; x < W; x++) {
				uint8_t* p = (uint8_t*)linMapped + sl.offset + y * sl.rowPitch + x * 4;
				if (x < W / 2) { p[0] = 64; p[1] = 128; p[2] = 191; p[3] = 255; } // (0.25,0.5,0.75)
				else { p[0] = 230; p[1] = 26; p[2] = 64; p[3] = 255; }              // (0.9,0.1,0.25)
			}
		vkUnmapMemory(dev, linMem);
	}

	// --- 6. 存储缓冲：float = 0.1（BDA，描述符走 pAddressRange 裸地址，与主工程一致）---
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = 256;
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer storeBuf;
	CHK(vkCreateBuffer(dev, &bci, nullptr, &storeBuf));
	VkDeviceMemory storeMem;
	uint64_t storeBufAddr = 0;
	{
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(dev, storeBuf, &req);
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		uint32_t mt = 0;
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) { mt = i; break; }
		// BDA buffer：内存须带 DEVICE_ADDRESS 位（VUID 03339）
		VkMemoryAllocateFlagsInfo flg = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
		flg.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.pNext = &flg;
		mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
		CHK(vkAllocateMemory(dev, &mai, nullptr, &storeMem));
		CHK(vkBindBufferMemory(dev, storeBuf, storeMem, 0));
		void* mp2;
		CHK(vkMapMemory(dev, storeMem, 0, 256, 0, &mp2));
		std::memcpy(mp2, &bufVal, sizeof(float));
		vkUnmapMemory(dev, storeMem);
		// BDA 查询须在内存绑定之后（VUID 03324）
		VkBufferDeviceAddressInfo bda = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
		bda.buffer = storeBuf;
		storeBufAddr = vkGetBufferDeviceAddress(dev, &bda);
	}

	// --- 7. descriptor heap（buffer + image 描述符混存，Q3）---
	uint32_t reservedSlots = (uint32_t)((hp.minResourceHeapReservedRange + slotSize - 1) / slotSize);
	uint32_t userSlots = 2; // slot0 = storage buffer, slot1 = sampled image
	uint32_t capacity = reservedSlots + userSlots;
	VkDeviceSize heapBytes = slotSize * capacity + hp.resourceHeapAlignment;
	VkBufferCreateInfo hci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	hci.size = heapBytes;
	hci.usage = VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	hci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer heapBuf;
	CHK(vkCreateBuffer(dev, &hci, nullptr, &heapBuf));
	VkDeviceMemory heapMem;
	uint8_t* heapMapped = nullptr;
	uint64_t heapGpuAddr = 0;
	uint64_t heapGpuOffset = 0;
	{
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(dev, heapBuf, &req);
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		uint32_t mt = 0;
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) { mt = i; break; }
		// descriptor heap 缓冲：内存必须带 DEVICE_ADDRESS 位（VUID 11408，主工程经 VMA 同规则）
		VkMemoryAllocateFlagsInfo flg = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
		flg.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.pNext = &flg;
		mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
		CHK(vkAllocateMemory(dev, &mai, nullptr, &heapMem));
		CHK(vkBindBufferMemory(dev, heapBuf, heapMem, 0));
		// BDA 查询须在内存绑定之后（VUID 03324）
		VkBufferDeviceAddressInfo bda = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
		bda.buffer = heapBuf;
		uint64_t rawAddr = vkGetBufferDeviceAddress(dev, &bda);
		// GPU 地址按 resourceHeapAlignment 对齐（与主工程 VmaDescriptorHeap 同模式）
		heapGpuOffset = (hp.resourceHeapAlignment - (rawAddr % hp.resourceHeapAlignment)) % hp.resourceHeapAlignment;
		heapGpuAddr = rawAddr + heapGpuOffset;
		// host 映射指针推进同样的偏移，使 host slot i 与 GPU slot i 对齐
		CHK(vkMapMemory(dev, heapMem, 0, heapBytes, 0, (void**)&heapMapped));
		heapMapped += heapGpuOffset;
	}
	fprintf(stderr, "heap: reservedSlots=%u userSlots=%u slotSize=%zu gpuAddr=0x%llx gpuOffset=%zu\n",
		reservedSlots, userSlots, slotSize, (unsigned long long)heapGpuAddr, heapGpuOffset);

	// --- 8. 写描述符：slot0 = STORAGE_BUFFER(pAddressRange), slot1 = SAMPLED_IMAGE(pImage) ---
	VkImageDescriptorInfoEXT imgDi = {};
	imgDi.sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT;
	imgDi.pView = &tvi;
	imgDi.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkDeviceAddressRangeEXT storeRange = {storeBufAddr, 256};
	VkResourceDescriptorInfoEXT infos[2] = {};
	infos[0].sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT;
	infos[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	infos[0].data.pAddressRange = &storeRange;
	infos[1].sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT;
	infos[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	infos[1].data.pImage = &imgDi;
	VkHostAddressRangeEXT hostRanges[2] = {
		{heapMapped + 0 * slotSize, (size_t)slotSize},
		{heapMapped + 1 * slotSize, (size_t)slotSize},
	};
	CHK(writeDescFn(dev, 2, infos, hostRanges));
	fprintf(stderr, "descriptors written: slot0=STORAGE_BUFFER slot1=SAMPLED_IMAGE (single resource heap, Q3)\n");

	// --- 9. shader object（mesh+frag 链接，内嵌 sampler Q4）---
	auto mspv = readBin("mesh.spv");
	auto fspv = readBin("frag.spv");
	if (mspv.empty() || fspv.empty()) { fprintf(stderr, "shader load failed\n"); return 1; }

	PFN_vkCreateShadersEXT vkCreateShadersEXT = (PFN_vkCreateShadersEXT)vkGetDeviceProcAddr(dev, "vkCreateShadersEXT");
	PFN_vkDestroyShaderEXT vkDestroyShaderEXT = (PFN_vkDestroyShaderEXT)vkGetDeviceProcAddr(dev, "vkDestroyShaderEXT");
	PFN_vkCmdBindShadersEXT vkCmdBindShadersEXT = (PFN_vkCmdBindShadersEXT)vkGetDeviceProcAddr(dev, "vkCmdBindShadersEXT");
	PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(dev, "vkCmdDrawMeshTasksEXT");
	PFN_vkCmdSetViewportWithCount vkSetVP = (PFN_vkCmdSetViewportWithCount)vkGetDeviceProcAddr(dev, "vkCmdSetViewportWithCount");
	PFN_vkCmdSetScissorWithCount vkSetSC = (PFN_vkCmdSetScissorWithCount)vkGetDeviceProcAddr(dev, "vkCmdSetScissorWithCount");
	PFN_vkCmdSetCullMode vkSetCull = (PFN_vkCmdSetCullMode)vkGetDeviceProcAddr(dev, "vkCmdSetCullMode");
	PFN_vkCmdSetFrontFace vkSetFF = (PFN_vkCmdSetFrontFace)vkGetDeviceProcAddr(dev, "vkCmdSetFrontFace");
	PFN_vkCmdSetLineWidth vkSetLW = (PFN_vkCmdSetLineWidth)vkGetDeviceProcAddr(dev, "vkCmdSetLineWidth");
	PFN_vkCmdSetDepthBiasEnable vkSetDBE = (PFN_vkCmdSetDepthBiasEnable)vkGetDeviceProcAddr(dev, "vkCmdSetDepthBiasEnable");
	PFN_vkCmdSetDepthTestEnable vkSetDTest = (PFN_vkCmdSetDepthTestEnable)vkGetDeviceProcAddr(dev, "vkCmdSetDepthTestEnable");
	PFN_vkCmdSetDepthWriteEnable vkSetDWr = (PFN_vkCmdSetDepthWriteEnable)vkGetDeviceProcAddr(dev, "vkCmdSetDepthWriteEnable");
	PFN_vkCmdSetPrimitiveTopology vkSetTopo = (PFN_vkCmdSetPrimitiveTopology)vkGetDeviceProcAddr(dev, "vkCmdSetPrimitiveTopology");
	PFN_vkCmdSetPrimitiveRestartEnable vkSetPRI = (PFN_vkCmdSetPrimitiveRestartEnable)vkGetDeviceProcAddr(dev, "vkCmdSetPrimitiveRestartEnable");
	PFN_vkCmdSetRasterizerDiscardEnable vkSetRD = (PFN_vkCmdSetRasterizerDiscardEnable)vkGetDeviceProcAddr(dev, "vkCmdSetRasterizerDiscardEnable");
	PFN_vkCmdSetBlendConstants vkSetBC = (PFN_vkCmdSetBlendConstants)vkGetDeviceProcAddr(dev, "vkCmdSetBlendConstants");
	PFN_vkCmdSetRasterizationSamplesEXT vkSetRSS = (PFN_vkCmdSetRasterizationSamplesEXT)vkGetDeviceProcAddr(dev, "vkCmdSetRasterizationSamplesEXT");
	PFN_vkCmdSetSampleMaskEXT vkSetSM = (PFN_vkCmdSetSampleMaskEXT)vkGetDeviceProcAddr(dev, "vkCmdSetSampleMaskEXT");
	PFN_vkCmdSetColorBlendEnableEXT vkSetCBE = (PFN_vkCmdSetColorBlendEnableEXT)vkGetDeviceProcAddr(dev, "vkCmdSetColorBlendEnableEXT");
	PFN_vkCmdSetColorWriteMaskEXT vkSetCWM = (PFN_vkCmdSetColorWriteMaskEXT)vkGetDeviceProcAddr(dev, "vkCmdSetColorWriteMaskEXT");
	PFN_vkCmdSetLogicOpEnableEXT vkSetLOG = (PFN_vkCmdSetLogicOpEnableEXT)vkGetDeviceProcAddr(dev, "vkCmdSetLogicOpEnableEXT");
	PFN_vkCmdSetDepthClampEnableEXT vkSetDCE = (PFN_vkCmdSetDepthClampEnableEXT)vkGetDeviceProcAddr(dev, "vkCmdSetDepthClampEnableEXT");
	PFN_vkCmdSetPolygonModeEXT vkSetPM = (PFN_vkCmdSetPolygonModeEXT)vkGetDeviceProcAddr(dev, "vkCmdSetPolygonModeEXT");
	PFN_vkCmdSetDepthBounds vkSetDB = (PFN_vkCmdSetDepthBounds)vkGetDeviceProcAddr(dev, "vkCmdSetDepthBounds");
	PFN_vkCmdSetDepthBoundsTestEnable vkSetDBTE = (PFN_vkCmdSetDepthBoundsTestEnable)vkGetDeviceProcAddr(dev, "vkCmdSetDepthBoundsTestEnable");
	PFN_vkCmdSetStencilTestEnable vkSetST = (PFN_vkCmdSetStencilTestEnable)vkGetDeviceProcAddr(dev, "vkCmdSetStencilTestEnable");
	PFN_vkCmdSetStencilOpEXT vkSetSTO = (PFN_vkCmdSetStencilOpEXT)vkGetDeviceProcAddr(dev, "vkCmdSetStencilOpEXT");
	PFN_vkCmdSetVertexInputEXT vkSetVI = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(dev, "vkCmdSetVertexInputEXT");
	PFN_vkCmdSetDepthBias vkSetBias = (PFN_vkCmdSetDepthBias)vkGetDeviceProcAddr(dev, "vkCmdSetDepthBias");
	PFN_vkCmdSetDepthCompareOp vkSetDComp = (PFN_vkCmdSetDepthCompareOp)vkGetDeviceProcAddr(dev, "vkCmdSetDepthCompareOp");
	PFN_vkCmdSetAlphaToCoverageEnableEXT vkSetATC = (PFN_vkCmdSetAlphaToCoverageEnableEXT)vkGetDeviceProcAddr(dev, "vkCmdSetAlphaToCoverageEnableEXT");
	PFN_vkCmdSetAlphaToOneEnableEXT vkSetATO = (PFN_vkCmdSetAlphaToOneEnableEXT)vkGetDeviceProcAddr(dev, "vkCmdSetAlphaToOneEnableEXT");
	PFN_vkCmdSetColorBlendEquationEXT vkSetCBEQ = (PFN_vkCmdSetColorBlendEquationEXT)vkGetDeviceProcAddr(dev, "vkCmdSetColorBlendEquationEXT");
	if (!vkCreateShadersEXT || !vkCmdBindShadersEXT || !vkCmdDrawMeshTasksEXT || !vkSetVP) {
		fprintf(stderr, "ext funcs missing\n");
		return 1;
	}

	// 内嵌 sampler（Q4）：linear + clamp
	VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.anisotropyEnable = VK_FALSE;
	sci.compareEnable = VK_FALSE;
	sci.minLod = 0.0f; sci.maxLod = 0.0f;
	sci.unnormalizedCoordinates = VK_FALSE;
	sci.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;

	// binding mapping（v341：pEmbeddedSampler 在 sourceData.constantOffset 内）：
	//   b0 = sampler2D (OpTypeSampledImage) -> heap slot1 (SAMPLED_IMAGE) + pEmbeddedSampler
	//   b1 = SSBO    (OpTypeBuffer)         -> heap slot0 (STORAGE_BUFFER, pAddressRange)
	VkDescriptorSetAndBindingMappingEXT mappings[2] = {};
	mappings[0].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
	mappings[0].descriptorSet = 0; mappings[0].firstBinding = 0; mappings[0].bindingCount = 1;
	mappings[0].resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
	mappings[0].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
	mappings[0].sourceData.constantOffset.heapOffset = (reservedSlots + 1) * (uint32_t)slotSize;
	mappings[0].sourceData.constantOffset.heapArrayStride = 0;
	mappings[0].sourceData.constantOffset.pEmbeddedSampler = &sci;
	mappings[0].sourceData.constantOffset.samplerHeapOffset = 0;
	mappings[0].sourceData.constantOffset.samplerHeapArrayStride = 0;

	mappings[1].sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
	mappings[1].descriptorSet = 0; mappings[1].firstBinding = 1; mappings[1].bindingCount = 1;
	mappings[1].resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
	mappings[1].source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
	mappings[1].sourceData.constantOffset.heapOffset = reservedSlots * (uint32_t)slotSize;
	mappings[1].sourceData.constantOffset.heapArrayStride = 0;

	VkShaderDescriptorSetAndBindingMappingInfoEXT mi = {};
	mi.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
	mi.mappingCount = 2;
	mi.pMappings = mappings;

	VkShaderCreateInfoEXT cis[2] = {};
	cis[0].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	cis[0].pNext = &mi;
	cis[0].flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT | VK_SHADER_CREATE_LINK_STAGE_BIT_EXT | VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
	cis[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
	cis[0].nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
	cis[0].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	cis[0].codeSize = mspv.size();
	cis[0].pCode = (const uint32_t*)mspv.data();
	cis[0].pName = "main";

	cis[1].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	cis[1].pNext = &mi;
	cis[1].flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT | VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
	cis[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	cis[1].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	cis[1].codeSize = fspv.size();
	cis[1].pCode = (const uint32_t*)fspv.data();
	cis[1].pName = "main";

	VkShaderEXT handles[2];
	CHK2(vkCreateShadersEXT(dev, 2, cis, nullptr, handles));
	fprintf(stderr, "shaders created (embedded sampler on b0, heap mappings b0+b1)\n");
	if (g_failed) return 1;
	ShaderHandle meshS = handles[0], fragS = handles[1];

	// --- 10. 渲染目标 + 回读 buffer ---
	VkImageCreateInfo rci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	rci.imageType = VK_IMAGE_TYPE_2D; rci.format = fmt;
	rci.extent = {W, H, 1}; rci.mipLevels = 1; rci.arrayLayers = 1;
	rci.samples = VK_SAMPLE_COUNT_1_BIT; rci.tiling = VK_IMAGE_TILING_OPTIMAL;
	rci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	rci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage rt;
	CHK(vkCreateImage(dev, &rci, nullptr, &rt));
	VkDeviceMemory rtMem;
	{
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, rt, &req);
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		uint32_t mt = 0;
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = i; break; }
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
		CHK(vkAllocateMemory(dev, &mai, nullptr, &rtMem));
		CHK(vkBindImageMemory(dev, rt, rtMem, 0));
	}
	VkImageViewCreateInfo rvi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	rvi.image = rt; rvi.viewType = VK_IMAGE_VIEW_TYPE_2D; rvi.format = fmt;
	rvi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView rtView;
	CHK(vkCreateImageView(dev, &rvi, nullptr, &rtView));

	VkBufferCreateInfo rbi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	rbi.size = (VkDeviceSize)W * H * 4;
	rbi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	rbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer readBuf;
	CHK(vkCreateBuffer(dev, &rbi, nullptr, &readBuf));
	VkDeviceMemory readMem;
	uint8_t* readMapped = nullptr;
	{
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(dev, readBuf, &req);
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		uint32_t mt = 0;
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
			if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) { mt = i; break; }
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
		CHK(vkAllocateMemory(dev, &mai, nullptr, &readMem));
		CHK(vkBindBufferMemory(dev, readBuf, readMem, 0));
		CHK(vkMapMemory(dev, readMem, 0, rbi.size, 0, (void**)&readMapped));
	}

	// --- 11. 命令录制 ---
	VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpci.queueFamilyIndex = gq;
	VkCommandPool pool;
	CHK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
	VkCommandBuffer cb;
	CHK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	CHK(vkCreateFence(dev, &fi, nullptr, &fence));

	CHK(vkResetCommandBuffer(cb, 0));
	VkCommandBufferBeginInfo cb2 = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CHK(vkBeginCommandBuffer(cb, &cb2));

	// 纹理填充：linear UNDEFINED -> GENERAL（host 已写入映射内存），再 copy 到 optimal(TRANSFER_DST)
	{
		VkImageMemoryBarrier2 bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		bar.image = linImg;
		bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		bar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		bar.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		bar.srcAccessMask = 0;
		bar.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &bar;
		vkCmdPipelineBarrier2(cb, &di);

		VkImageCopy ic = {};
		ic.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		ic.extent = {W, H, 1};
		vkCmdCopyImage(cb, linImg, VK_IMAGE_LAYOUT_GENERAL, texImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	}
	// 纹理 TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL（描述符引用的 layout）
	{
		VkImageMemoryBarrier2 bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		bar.image = texImg;
		bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		bar.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		bar.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		bar.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &bar;
		vkCmdPipelineBarrier2(cb, &di);
	}
	// heap 缓冲 + 存储缓冲对 GPU 可见（写描述符/写 buffer 是 host 写，需 memory barrier）
	{
		// heap 缓冲（host 写描述符）+ 存储缓冲（host 写数据）对 GPU 可见
		VkBufferMemoryBarrier2 bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
		bb.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		bb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		bb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
		bb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
		bb.buffer = heapBuf;
		bb.size = VK_WHOLE_SIZE;
		VkBufferMemoryBarrier2 bb2 = bb;
		bb2.buffer = storeBuf;
		VkDependencyInfo dib = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		dib.bufferMemoryBarrierCount = 2;
		VkBufferMemoryBarrier2 bbs[2] = {bb, bb2};
		dib.pBufferMemoryBarriers = bbs;
		vkCmdPipelineBarrier2(cb, &dib);
	}

	// 绑定 resource heap（reserved range 声明不可用）
	{
		VkDeviceAddressRangeEXT hr = {};
		hr.address = heapGpuAddr;
		hr.size = heapBytes - heapGpuOffset;
		VkBindHeapInfoEXT bi = {};
		bi.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT;
		bi.heapRange = hr;
		bi.reservedRangeOffset = reservedSlots * (uint64_t)slotSize;
		bi.reservedRangeSize = reservedSlots * (uint64_t)slotSize;
		bindHeapFn(cb, &bi);
	}

	// 渲染目标 UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
	{
		VkImageMemoryBarrier2 bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		bar.image = rt;
		bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		bar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		bar.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		bar.srcAccessMask = 0;
		bar.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &bar;
		vkCmdPipelineBarrier2(cb, &di);
	}

	// 动态状态（shader object 要求完整设置，模式同 min_mesh_crash）
	{
		VkViewport vp = {0.0f, (float)H, (float)W, -(float)H, 0.0f, 1.0f};
		VkRect2D sc = {0, 0, {W, H}};
		float bcf[4] = {0, 0, 0, 0};
		VkSampleMask sm = ~0U;
		VkBool32 cbEn = VK_FALSE;
		VkColorComponentFlags cmask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkColorBlendEquationEXT cbeq = {};
		cbeq.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cbeq.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		cbeq.colorBlendOp = VK_BLEND_OP_ADD;
		cbeq.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cbeq.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		cbeq.alphaBlendOp = VK_BLEND_OP_ADD;

		vkSetVP(cb, 1, &vp);
		vkSetSC(cb, 1, &sc);
		vkSetLW(cb, 1.0f);
		vkSetCull(cb, VK_CULL_MODE_NONE);
		vkSetFF(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
		vkSetDBE(cb, VK_FALSE);
		vkSetRD(cb, VK_FALSE);
		vkSetDCE(cb, VK_FALSE);
		vkSetPM(cb, VK_POLYGON_MODE_FILL);
		vkSetBC(cb, bcf);
		vkSetDB(cb, 0.0f, 1.0f);
		vkSetDBTE(cb, VK_FALSE);
		vkSetDTest(cb, VK_FALSE);
		vkSetDWr(cb, VK_FALSE);
		vkSetST(cb, VK_FALSE);
		vkSetPRI(cb, VK_FALSE);
		vkSetTopo(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		vkSetRSS(cb, VK_SAMPLE_COUNT_1_BIT);
		vkSetSM(cb, VK_SAMPLE_COUNT_1_BIT, &sm);
		vkSetCBE(cb, 0, 1, &cbEn);
		vkSetCWM(cb, 0, 1, &cmask);
		vkSetLOG(cb, VK_FALSE);
		vkSetVI(cb, 0, nullptr, 0, nullptr);
		vkSetDComp(cb, VK_COMPARE_OP_LESS);
		vkSetATC(cb, VK_FALSE);
		vkSetATO(cb, VK_FALSE);
		vkSetCBEQ(cb, 0, 1, &cbeq);
		vkSetSTO(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
		vkSetBias(cb, 0.0f, 0.0f, 0.0f);
	}

	// beginRendering + draw
	{
		VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		ca.imageView = rtView;
		ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ca.clearValue.color.float32[0] = 0.05f;
		ca.clearValue.color.float32[1] = 0.05f;
		ca.clearValue.color.float32[2] = 0.05f;
		ca.clearValue.color.float32[3] = 1.0f;
		VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, {W, H}};
		ri.layerCount = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments = &ca;
		vkCmdBeginRendering(cb, &ri);

		VkShaderStageFlagBits stages[] = {
			VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
			VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
			VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT, VK_SHADER_STAGE_FRAGMENT_BIT};
		VkShaderEXT sh[] = {
			VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
			VK_NULL_HANDLE, (VkShaderEXT)meshS, (VkShaderEXT)fragS};
		vkCmdBindShadersEXT(cb, 7, stages, sh);
		vkCmdDrawMeshTasksEXT(cb, 1, 1, 1);
		vkCmdEndRendering(cb);
	}

	// 渲染目标 COLOR_ATTACHMENT -> TRANSFER_SRC，copy 回读 buffer
	{
		VkImageMemoryBarrier2 bar = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
		bar.image = rt;
		bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		bar.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		bar.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		bar.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		bar.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &bar;
		vkCmdPipelineBarrier2(cb, &di);

		VkBufferImageCopy bic = {};
		bic.bufferOffset = 0;
		bic.bufferRowLength = 0;
		bic.bufferImageHeight = H;
		bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		bic.imageOffset = {0, 0, 0};
		bic.imageExtent = {W, H, 1};
		vkCmdCopyImageToBuffer(cb, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuf, 1, &bic);
	}
	CHK(vkEndCommandBuffer(cb));

	VkSubmitInfo sb = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	sb.commandBufferCount = 1;
	sb.pCommandBuffers = &cb;
	VkResult sr = vkQueueSubmit(q, 1, &sb, fence);
	if (sr != VK_SUCCESS) {
		fprintf(stderr, "submit failed: %d\n", (int)sr);
		g_failed = true;
	} else {
		CHK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ULL));
	}

	// --- 12. 像素验证 ---
	fprintf(stderr, "\n=== pixel verification ===\n");
	if (!g_failed) {
		// 纹理左半 (0.25,0.5,0.75) + bufVal 0.1 = (0.35,0.6,0.85)；右半 (0.9,0.1,0.25) + 0.1 = (1.0,0.2,0.35)
		struct Check { uint32_t x, y; float r, g, b; const char* name; };
		Check checks[] = {
			{8, 8, 0.35f, 0.6f, 0.85f, "top-left (left half + buf)"},
			{W / 2 - 8, H / 2, 0.35f, 0.6f, 0.85f, "center-left"},
			{W / 2 + 8, H / 2, 1.0f, 0.2f, 0.35f, "center-right (0.9+0.1 clamped to 1.0)"},
			{W - 8, H - 8, 1.0f, 0.2f, 0.35f, "bottom-right"},
		};
		for (auto& c : checks) {
			const uint8_t* px = readMapped + (c.y * W + c.x) * 4;
			bool ok = pixelClose(px, c.r, c.g, c.b, 1.0f);
			fprintf(stderr, "  %-32s px(%u,%u)=(%u,%u,%u,%u) expected(%.2f,%.2f,%.2f,1.0) %s\n",
				c.name, c.x, c.y, px[0], px[1], px[2], px[3], c.r, c.g, c.b, ok ? "OK" : "MISMATCH");
			if (!ok) g_failed = true;
		}
	}

	// --- 13. 结论 ---
	fprintf(stderr, "\n=== RESULT ===\n");
	fprintf(stderr, "Q1 imageDescSize<=bufferDescSize: %s\n", q1ok ? "PASS" : "FAIL");
	fprintf(stderr, "Q2 slot stride alignment:        %s\n", q2ok ? "PASS" : "FAIL");
	fprintf(stderr, "Q3 buffer+image in one heap:     %s\n", g_failed ? "FAIL" : "PASS");
	fprintf(stderr, "Q4 embedded sampler sampling:    %s\n", g_failed ? "FAIL" : "PASS");
	fprintf(stderr, "OVERALL: %s\n", g_failed ? "FAIL" : "PASS");

	// --- cleanup ---
	vkDeviceWaitIdle(dev);
	vkDestroyShaderEXT(dev, (VkShaderEXT)meshS, nullptr);
	vkDestroyShaderEXT(dev, (VkShaderEXT)fragS, nullptr);
	vkUnmapMemory(dev, readMem);
	vkFreeMemory(dev, readMem, nullptr);
	vkDestroyBuffer(dev, readBuf, nullptr);
	vkDestroyImageView(dev, rtView, nullptr);
	vkDestroyImage(dev, rt, nullptr);
	vkFreeMemory(dev, rtMem, nullptr);
	vkDestroyImageView(dev, texView, nullptr);
	vkDestroyImage(dev, texImg, nullptr);
	vkFreeMemory(dev, texMem, nullptr);
	vkUnmapMemory(dev, heapMem);
	vkFreeMemory(dev, heapMem, nullptr);
	vkDestroyBuffer(dev, heapBuf, nullptr);
	vkFreeMemory(dev, storeMem, nullptr);
	vkDestroyBuffer(dev, storeBuf, nullptr);
	// linMem 已在填图作用域内 unmap
	vkFreeMemory(dev, linMem, nullptr);
	vkDestroyImage(dev, linImg, nullptr);
	vkDestroyFence(dev, fence, nullptr);
	vkDestroyCommandPool(dev, pool, nullptr);
	vkDestroyDevice(dev, nullptr);
	vkDestroyInstance(inst, nullptr);
	return g_failed ? 1 : 0;
}
