#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "GFSDK_Aftermath_GpuCrashDump.h"
#include "GFSDK_Aftermath_GpuCrashDumpDecoding.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>

#define CHK(r) do{VkResult _r=(r);if(_r!=VK_SUCCESS){fprintf(stderr,"FAIL %s=%d %d\n",#r,(int)_r,__LINE__);return 1;}}while(0)

static void GFSDK_AFTERMATH_CALL dumpCb(const void* d, uint32_t s, void*) {
    FILE* f=fopen("min_dump.bin","wb"); if(f){fwrite(d,1,s,f);fclose(f);}
    fprintf(stderr,"\n[Aftermath] dump %d bytes\n",s);
    GFSDK_Aftermath_GpuCrashDump_Decoder dc=nullptr;
    auto rc=GFSDK_Aftermath_GpuCrashDump_CreateDecoder(GFSDK_Aftermath_Version_API,d,s,&dc);
    if(GFSDK_Aftermath_SUCCEED(rc)){
        GFSDK_Aftermath_GpuCrashDump_DeviceInfo di={};
        GFSDK_Aftermath_GpuCrashDump_GetDeviceInfo(dc,&di);
        fprintf(stderr,"[Aftermath] status=%d\n",(int)di.status);
        uint32_t sc=0; GFSDK_Aftermath_GpuCrashDump_GetActiveShadersInfoCount(dc,&sc);
        fprintf(stderr,"[Aftermath] shaders=%d\n",sc);
        uint32_t mc=0; GFSDK_Aftermath_GpuCrashDump_GetEventMarkersInfoCount(dc,&mc);
        fprintf(stderr,"[Aftermath] markers=%d\n",mc);
        uint32_t js=0;
        GFSDK_Aftermath_GpuCrashDump_GenerateJSON(dc,0x3fff,2,nullptr,nullptr,nullptr,nullptr,&js);
        if(js>0){std::vector<char> b(js);GFSDK_Aftermath_GpuCrashDump_GetJSON(dc,js,b.data());
            FILE* jf=fopen("min_dump.json","wb");if(jf){fwrite(b.data(),1,js,jf);fclose(jf);}}
        GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(dc);
    }
}

static std::vector<uint8_t> readBin(const char* p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);
    auto e=f.tellg(); if(e<=0){fprintf(stderr,"cannot read %s\n",p);return {};}
    f.seekg(0);
    std::vector<uint8_t> b((size_t)e); f.read((char*)b.data(),e); return b;
}

using ShaderHandle=void*;
#define GPA(name) (PFN_##name)vkGetDeviceProcAddr(dev,#name)

int main() {
    GFSDK_Aftermath_EnableGpuCrashDumps(GFSDK_Aftermath_Version_API,
        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
        dumpCb,nullptr,nullptr,nullptr,nullptr);

    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* win=SDL_CreateWindow("m",640,480,SDL_WINDOW_VULKAN);
    if(!win){fprintf(stderr,"SDL fail: %s\n",SDL_GetError());return 1;}

    VkInstance inst;
    VkApplicationInfo ai={VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName="m"; ai.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo=&ai;
    uint32_t extCount;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    std::vector<const char*> allExts;
    for(uint32_t i=0;i<extCount;i++) allExts.push_back(exts[i]);
    allExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    ic.enabledExtensionCount=allExts.size(); ic.ppEnabledExtensionNames=allExts.data();
    for(auto e:allExts) fprintf(stderr,"  inst ext: %s\n",e);
    CHK(vkCreateInstance(&ic,nullptr,&inst));

    VkSurfaceKHR surf;
    if(!SDL_Vulkan_CreateSurface(win,inst,nullptr,&surf)){fprintf(stderr,"surf fail: %s\n",SDL_GetError());return 1;}
    fprintf(stderr,"surface created\n");

    uint32_t pc; vkEnumeratePhysicalDevices(inst,&pc,nullptr);
    std::vector<VkPhysicalDevice> pds(pc);
    vkEnumeratePhysicalDevices(inst,&pc,pds.data());
    VkPhysicalDevice pd=nullptr;
    for(uint32_t i=0;i<pc;i++){
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i],&p);
        fprintf(stderr,"  GPU%d: %s (vendor=0x%x)\n",i,p.deviceName,p.vendorID);
        if(p.vendorID==0x10de){pd=pds[i];break;}
    }
    if(!pd){pd=pds[0];}
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd,&pp);
    fprintf(stderr,"Selected: %s\n",pp.deviceName);

    const char* de[]={
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
         VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
         VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
         VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
        VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
        VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME,
        VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME,
    };

    VkPhysicalDeviceDescriptorHeapPropertiesEXT hp={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext=&hp; vkGetPhysicalDeviceProperties2(pd,&p2);
    fprintf(stderr,"heap: dsz=%u res=%u aln=%u\n",hp.bufferDescriptorSize,(uint32_t)hp.minResourceHeapReservedRange,(uint32_t)hp.resourceHeapAlignment);

    uint32_t qfc; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfc);
    vkGetPhysicalDeviceQueueFamilyProperties(pd,&qfc,qfs.data());
    uint32_t gq=0;
    VkBool32 presentSupport=VK_FALSE;
    for(uint32_t i=0;i<qfc;i++){
        VkBool32 p=VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(pd,i,surf,&p);
        if((qfs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){
            gq=i;
            if(p) presentSupport=VK_TRUE;
        }
    }
    if(!presentSupport){
        fprintf(stderr,"WARN: selected GPU cannot present!\n");
    }

    float pq=1;
    VkDeviceQueueCreateInfo dq={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex=gq; dq.queueCount=1; dq.pQueuePriorities=&pq;

    VkDeviceDiagnosticsConfigCreateInfoNV dgc={VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV};
    dgc.flags=VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV
            |VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV
            |VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV
            |VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV;

    // Feature chain: ALL required features must be enabled
    VkPhysicalDeviceVulkan12Features f12={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress=VK_TRUE;

    VkPhysicalDeviceVulkan13Features f13={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering=VK_TRUE;
    f13.synchronization2=VK_TRUE;
    f13.maintenance4=VK_TRUE;

    VkPhysicalDeviceMeshShaderFeaturesEXT mf={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    mf.meshShader=VK_TRUE;

    VkPhysicalDeviceDescriptorHeapFeaturesEXT hf={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    hf.descriptorHeap=VK_TRUE;

    VkPhysicalDeviceShaderObjectFeaturesEXT sof={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    sof.shaderObject=VK_TRUE;

    // pNext chain: dgc → f2 → f12 → f13 → mf → hf → sof
    VkPhysicalDeviceFeatures2 f2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    dgc.pNext=&f2; f2.pNext=&f12; f12.pNext=&f13; f13.pNext=&mf; mf.pNext=&hf; hf.pNext=&sof;

    VkDeviceCreateInfo dc2={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc2.pNext=&dgc; dc2.queueCreateInfoCount=1; dc2.pQueueCreateInfos=&dq;
    dc2.enabledExtensionCount=sizeof(de)/sizeof(*de); dc2.ppEnabledExtensionNames=de;
    VkDevice dev; CHK(vkCreateDevice(pd,&dc2,nullptr,&dev));
    fprintf(stderr,"features: shaderObject=%d dynamicRendering=%d sync2=%d bufferDeviceAddr=%d meshShader=%d descriptorHeap=%d\n",
            sof.shaderObject, f13.dynamicRendering, f13.synchronization2, f12.bufferDeviceAddress, mf.meshShader, hf.descriptorHeap);

    VkQueue q; vkGetDeviceQueue(dev,gq,0,&q);

    // Offscreen render target (no swapchain needed)
    VkFormat fmt=VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D ex={640,480};
    VkImageCreateInfo imgci={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgci.imageType=VK_IMAGE_TYPE_2D; imgci.format=fmt; imgci.extent={ex.width,ex.height,1};
    imgci.mipLevels=1; imgci.arrayLayers=1; imgci.samples=VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling=VK_IMAGE_TILING_OPTIMAL; imgci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage tgt; CHK(vkCreateImage(dev,&imgci,nullptr,&tgt));
    fprintf(stderr,"image created\n");

    // Bind memory BEFORE creating image view
    VkDeviceMemory imgMem;
    {
        VkMemoryRequirements imgReqs; vkGetImageMemoryRequirements(dev,tgt,&imgReqs);
        VkPhysicalDeviceMemoryProperties imgMp; vkGetPhysicalDeviceMemoryProperties(pd,&imgMp);
        uint32_t imgMt = 0;
        for(uint32_t i=0;i<imgMp.memoryTypeCount;i++){
            if((imgReqs.memoryTypeBits&(1u<<i))&&
               (imgMp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){
                imgMt=i; break;
            }
        }
        VkMemoryAllocateInfo imgMai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        imgMai.allocationSize=imgReqs.size; imgMai.memoryTypeIndex=imgMt;
        CHK(vkAllocateMemory(dev,&imgMai,nullptr,&imgMem));
        CHK(vkBindImageMemory(dev,tgt,imgMem,0));
        fprintf(stderr,"image memory bound ok (type=%u size=%zu)\n",imgMt,imgReqs.size);
    }

    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=tgt; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=fmt;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView tgtView; CHK(vkCreateImageView(dev,&vi,nullptr,&tgtView));
    fprintf(stderr,"view created\n");

    // No descriptor heap needed — mesh shader uses no resources
    fprintf(stderr,"shaders loading\n");
    auto mspv=readBin("mesh.spv"); auto fspv=readBin("frag.spv");
    auto vspv=readBin("vert.spv");
    fprintf(stderr,"shaders loaded mesh=%zu vert=%zu frag=%zu\n",mspv.size(),vspv.size(),fspv.size());
    if(mspv.empty()||fspv.empty()||vspv.empty()) return 1;

    PFN_vkCreateShadersEXT vkCreateShadersEXT=GPA(vkCreateShadersEXT);
    PFN_vkDestroyShaderEXT vkDestroyShaderEXT=GPA(vkDestroyShaderEXT);
    PFN_vkCmdBindShadersEXT vkCmdBindShadersEXT=GPA(vkCmdBindShadersEXT);
    PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT=GPA(vkCmdDrawMeshTasksEXT);
    if(!vkCreateShadersEXT||!vkCmdBindShadersEXT||!vkCmdDrawMeshTasksEXT){
        fprintf(stderr,"ext funcs missing\n"); return 1;
    }

    // Load all required dynamic state function pointers
    PFN_vkCmdSetViewportWithCount vkSetVP=GPA(vkCmdSetViewportWithCount);
    PFN_vkCmdSetScissorWithCount vkSetSC=GPA(vkCmdSetScissorWithCount);
    PFN_vkCmdSetCullMode vkSetCull=GPA(vkCmdSetCullMode);
    PFN_vkCmdSetFrontFace vkSetFF=GPA(vkCmdSetFrontFace);
    PFN_vkCmdSetLineWidth vkSetLW=GPA(vkCmdSetLineWidth);
    PFN_vkCmdSetDepthBiasEnable vkSetDBE=GPA(vkCmdSetDepthBiasEnable);
    PFN_vkCmdSetDepthTestEnable vkSetDTest=GPA(vkCmdSetDepthTestEnable);
    PFN_vkCmdSetDepthWriteEnable vkSetDWr=GPA(vkCmdSetDepthWriteEnable);
    PFN_vkCmdSetPrimitiveTopology vkSetTopo=GPA(vkCmdSetPrimitiveTopology);
    PFN_vkCmdSetPrimitiveRestartEnable vkSetPRI=GPA(vkCmdSetPrimitiveRestartEnable);
    PFN_vkCmdSetRasterizerDiscardEnable vkSetRD=GPA(vkCmdSetRasterizerDiscardEnable);
    PFN_vkCmdSetBlendConstants vkSetBC=GPA(vkCmdSetBlendConstants);
    PFN_vkCmdSetRasterizationSamplesEXT vkSetRSS=GPA(vkCmdSetRasterizationSamplesEXT);
    PFN_vkCmdSetSampleMaskEXT vkSetSM=GPA(vkCmdSetSampleMaskEXT);
    PFN_vkCmdSetColorBlendEnableEXT vkSetCBE=GPA(vkCmdSetColorBlendEnableEXT);
    PFN_vkCmdSetColorWriteMaskEXT vkSetCWM=GPA(vkCmdSetColorWriteMaskEXT);
    PFN_vkCmdSetLogicOpEnableEXT vkSetLOG=GPA(vkCmdSetLogicOpEnableEXT);
    PFN_vkCmdSetDepthClampEnableEXT vkSetDCE=GPA(vkCmdSetDepthClampEnableEXT);
    PFN_vkCmdSetPolygonModeEXT vkSetPM=GPA(vkCmdSetPolygonModeEXT);
    PFN_vkCmdSetDepthBounds vkSetDB=GPA(vkCmdSetDepthBounds);
    PFN_vkCmdSetDepthBoundsTestEnable vkSetDBTE=GPA(vkCmdSetDepthBoundsTestEnable);
    PFN_vkCmdSetStencilTestEnable vkSetST=GPA(vkCmdSetStencilTestEnable);
    PFN_vkCmdSetStencilOpEXT vkSetStencilOp=GPA(vkCmdSetStencilOpEXT);
    PFN_vkCmdSetVertexInputEXT vkSetVI=GPA(vkCmdSetVertexInputEXT);
    PFN_vkCmdSetDepthBias vkSetBias=GPA(vkCmdSetDepthBias);
    PFN_vkCmdSetDepthCompareOp vkSetDComp=GPA(vkCmdSetDepthCompareOp);
    PFN_vkCmdSetAlphaToCoverageEnableEXT vkSetATC=GPA(vkCmdSetAlphaToCoverageEnableEXT);
    PFN_vkCmdSetAlphaToOneEnableEXT vkSetATO=GPA(vkCmdSetAlphaToOneEnableEXT);
    PFN_vkCmdSetColorBlendEquationEXT vkSetCBEQ=GPA(vkCmdSetColorBlendEquationEXT);

    if(!vkSetVP||!vkSetSC||!vkSetCull||!vkSetFF||!vkSetRSS||!vkSetCBE||!vkSetCWM||!vkSetDComp||!vkSetATC||!vkSetATO||!vkSetCBEQ){
        fprintf(stderr,"WARNING: some dynamic state funcs NULL!\n");
    }

    // Create BOTH shaders in a single vkCreateShadersEXT call (required for
    // LINK_STAGE_BIT_EXT to work — the driver links mesh<->fragment stages)
    VkShaderCreateInfoEXT createInfos[2]={};
    createInfos[0].sType=VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
    createInfos[0].flags=VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
    createInfos[0].flags|=VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
    createInfos[0].stage=VK_SHADER_STAGE_MESH_BIT_EXT;
    createInfos[0].nextStage=VK_SHADER_STAGE_FRAGMENT_BIT;
    createInfos[0].codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT;
    createInfos[0].codeSize=mspv.size();
    createInfos[0].pCode=(const uint32_t*)mspv.data();
    createInfos[0].pName="main";

    createInfos[1].sType=VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
    createInfos[1].flags=VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
    createInfos[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
    createInfos[1].codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT;
    createInfos[1].codeSize=fspv.size();
    createInfos[1].pCode=(const uint32_t*)fspv.data();
    createInfos[1].pName="main";

    VkShaderEXT shaderHandles[2];
    CHK(vkCreateShadersEXT(dev,2,createInfos,nullptr,shaderHandles));
    ShaderHandle meshS=shaderHandles[0];
    ShaderHandle fragS=shaderHandles[1];
    fprintf(stderr,"shaders created ok\n");

    // CB
    fprintf(stderr,"creating cmd pool\n");
    VkCommandPoolCreateInfo cpci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpci.queueFamilyIndex=gq;
    VkCommandPool pool; CHK(vkCreateCommandPool(dev,&cpci,nullptr,&pool));
    VkCommandBufferAllocateInfo cai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool=pool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    VkCommandBuffer cb; CHK(vkAllocateCommandBuffers(dev,&cai,&cb));
    fprintf(stderr,"cmd buf allocated\n");

    VkFenceCreateInfo fi={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence; CHK(vkCreateFence(dev,&fi,nullptr,&fence));

    // Shared dynamic state variables
    float bcf[4]={0,0,0,0};
    VkSampleMask sm=~0U;
    VkBool32 cbEn=VK_FALSE;
    VkColorComponentFlags cmask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkColorBlendEquationEXT cbeq{};
    cbeq.srcColorBlendFactor=VK_BLEND_FACTOR_ONE;
    cbeq.dstColorBlendFactor=VK_BLEND_FACTOR_ZERO;
    cbeq.colorBlendOp=VK_BLEND_OP_ADD;
    cbeq.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
    cbeq.dstAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;
    cbeq.alphaBlendOp=VK_BLEND_OP_ADD;

    // Shared dynamic state variables
    VkViewport vp={0.0f,(float)ex.height,(float)ex.width,-(float)ex.height,0.0f,1.0f};
    VkRect2D sc={0,0,ex};
    VkImageMemoryBarrier2 imgBar={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkImageMemoryBarrier2 bar={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo di={VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    imgBar.image=tgt; imgBar.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    imgBar.srcQueueFamilyIndex=imgBar.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    imgBar.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    imgBar.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imgBar.srcAccessMask=0; imgBar.dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    imgBar.srcStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    imgBar.dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkDependencyInfo imgDi={VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    imgDi.imageMemoryBarrierCount=1; imgDi.pImageMemoryBarriers=&imgBar;

    // === VERTEX+FRAGMENT TEST (no mesh shader) ===
    fprintf(stderr,"=== Vertex+Fragment shader object test ===\n");
    {
        // Own vert+frag pair (separate create call) so the frag shader is not
        // LINK_STAGE-linked to the mesh shader (VUID-vkCmdDraw-None-08698)
        VkShaderCreateInfoEXT vci[2]={};
        vci[0].sType=VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        vci[0].flags=VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
        vci[0].stage=VK_SHADER_STAGE_VERTEX_BIT;
        vci[0].nextStage=VK_SHADER_STAGE_FRAGMENT_BIT;
        vci[0].codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT;
        vci[0].codeSize=vspv.size();
        vci[0].pCode=(const uint32_t*)vspv.data();
        vci[0].pName="main";
        vci[1].sType=VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        vci[1].flags=VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
        vci[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
        vci[1].codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT;
        vci[1].codeSize=fspv.size();
        vci[1].pCode=(const uint32_t*)fspv.data();
        vci[1].pName="main";
        VkShaderEXT vhandles[2];
        CHK(vkCreateShadersEXT(dev,2,vci,nullptr,vhandles));
        VkShaderEXT vertS=vhandles[0],vfragS=vhandles[1];

        CHK(vkWaitForFences(dev,1,&fence,VK_TRUE,50000000000ULL));
        CHK(vkResetFences(dev,1,&fence));
        CHK(vkResetCommandBuffer(cb,0));
        VkCommandBufferBeginInfo cb2={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHK(vkBeginCommandBuffer(cb,&cb2));
        vkCmdPipelineBarrier2(cb,&imgDi);

        VkViewport vp2={0.0f,(float)ex.height,(float)ex.width,-(float)ex.height,0.0f,1.0f};
        vkSetVP(cb,1,&vp2);
        VkRect2D sc2={0,0,ex};
        vkSetSC(cb,1,&sc2);
        vkSetLW(cb,1.0f);
        vkSetCull(cb,VK_CULL_MODE_NONE);
        vkSetFF(cb,VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkSetDBE(cb,VK_FALSE);
        vkSetRD(cb,VK_FALSE);
        vkSetDCE(cb,VK_FALSE);
        vkSetPM(cb,VK_POLYGON_MODE_FILL);
        vkSetBC(cb,bcf);
        vkSetDB(cb,0.0f,1.0f);
        vkSetDBTE(cb,VK_FALSE);
        vkSetDTest(cb,VK_FALSE);
        vkSetDWr(cb,VK_FALSE);
        vkSetST(cb,VK_FALSE);
        vkSetPRI(cb,VK_FALSE);
        vkSetTopo(cb,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkSetRSS(cb,VK_SAMPLE_COUNT_1_BIT);
        vkSetSM(cb,VK_SAMPLE_COUNT_1_BIT,&sm);
        vkSetCBE(cb,0,1,&cbEn);
        vkSetCWM(cb,0,1,&cmask);
        vkSetLOG(cb,VK_FALSE);
        vkSetVI(cb,0,nullptr,0,nullptr);
        vkSetDComp(cb,VK_COMPARE_OP_LESS);
        vkSetATC(cb,VK_FALSE);
        vkSetATO(cb,VK_FALSE);
        vkSetCBEQ(cb,0,1,&cbeq);
        vkSetStencilOp(cb,VK_STENCIL_FACE_FRONT_AND_BACK,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS);
        vkSetBias(cb,0.0f,0.0f,0.0f);

        VkRenderingAttachmentInfo ca2={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca2.imageView=tgtView; ca2.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca2.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ca2.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        ca2.clearValue.color.float32[0]=0.5f;
        VkRenderingInfo ri2={VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri2.renderArea={{0,0},ex}; ri2.layerCount=1;
        ri2.colorAttachmentCount=1; ri2.pColorAttachments=&ca2;

        VkShaderStageFlagBits vstages[]={
            VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,VK_SHADER_STAGE_GEOMETRY_BIT,
            VK_SHADER_STAGE_TASK_BIT_EXT,VK_SHADER_STAGE_MESH_BIT_EXT,
            VK_SHADER_STAGE_FRAGMENT_BIT};
        VkShaderEXT vbShaders[]={
            vertS,VK_NULL_HANDLE,VK_NULL_HANDLE,VK_NULL_HANDLE,
            VK_NULL_HANDLE,VK_NULL_HANDLE,(VkShaderEXT)vfragS};
        vkCmdBindShadersEXT(cb,7,vstages,vbShaders);
        vkCmdBeginRendering(cb,&ri2);
        vkCmdDraw(cb,3,1,0,0);
        vkCmdEndRendering(cb);
        CHK(vkEndCommandBuffer(cb));

        VkSubmitInfo sbi={VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sbi.commandBufferCount=1; sbi.pCommandBuffers=&cb;
        VkResult vres=vkQueueSubmit(q,1,&sbi,fence);
        if(vres!=VK_SUCCESS){
            fprintf(stderr,"VERT_TEST submit failed: %d\n",(int)vres);
        } else {
            CHK(vkWaitForFences(dev,1,&fence,VK_TRUE,50000000000ULL));
            fprintf(stderr,"VERT_TEST: SUCCESS - vertex+fragment shader objects work!\n");
        }
        vkDestroyShaderEXT(dev,vertS,nullptr);
        vkDestroyShaderEXT(dev,vfragS,nullptr);
    }

    fprintf(stderr,"=== Mesh shader test ===\n");
    for(int f=0;f<10;f++){
        fprintf(stderr,"frame %d start\n",f);
        VkImageLayout oldL=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // Wait for previous frame to finish before reusing command buffer/fence
        CHK(vkWaitForFences(dev,1,&fence,VK_TRUE,50000000000ULL));
        CHK(vkResetFences(dev,1,&fence));
        CHK(vkResetCommandBuffer(cb,0));
        VkCommandBufferBeginInfo cbb={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHK(vkBeginCommandBuffer(cb,&cbb));

        // Image barrier: color attachment (already transitioned by vert test)
        bar.image=tgt; bar.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        bar.srcQueueFamilyIndex=bar.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        bar.oldLayout=oldL; bar.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.srcAccessMask=0; bar.dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        bar.srcStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        bar.dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        di.imageMemoryBarrierCount=1; di.pImageMemoryBarriers=&bar;
        vkCmdPipelineBarrier2(cb,&di);

        // === SET ALL DYNAMIC STATE (required for shader objects) ===
        vkSetVP(cb,1,&vp);
        vkSetSC(cb,1,&sc);
        vkSetLW(cb,1.0f);
        vkSetCull(cb,VK_CULL_MODE_NONE);
        vkSetFF(cb,VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkSetDBE(cb,VK_FALSE);
        vkSetRD(cb,VK_FALSE);
        vkSetDCE(cb,VK_FALSE);
        vkSetPM(cb,VK_POLYGON_MODE_FILL);
        vkSetBC(cb,bcf);
        vkSetDB(cb,0.0f,1.0f);
        vkSetDBTE(cb,VK_FALSE);
        vkSetDTest(cb,VK_FALSE);
        vkSetDWr(cb,VK_FALSE);
        vkSetST(cb,VK_FALSE);
        vkSetPRI(cb,VK_FALSE);
        vkSetTopo(cb,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkSetRSS(cb,VK_SAMPLE_COUNT_1_BIT);
        vkSetSM(cb,VK_SAMPLE_COUNT_1_BIT,&sm);
        vkSetCBE(cb,0,1,&cbEn);
        vkSetCWM(cb,0,1,&cmask);
        vkSetLOG(cb,VK_FALSE);
        vkSetBias(cb,0.0f,0.0f,0.0f);
        vkSetDComp(cb,VK_COMPARE_OP_LESS);
        vkSetATC(cb,VK_FALSE);
        vkSetATO(cb,VK_FALSE);
        vkSetCBEQ(cb,0,1,&cbeq);
        vkSetStencilOp(cb,VK_STENCIL_FACE_FRONT_AND_BACK,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS);
        vkSetVI(cb,0,nullptr,0,nullptr);

        VkRenderingAttachmentInfo ca={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView=tgtView; ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color.float32[0]=0.1f;
        VkRenderingInfo ri={VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea={{0,0},ex}; ri.layerCount=1;
        ri.colorAttachmentCount=1; ri.pColorAttachments=&ca;

        // Bind ALL 7 shader stages — unused stages must be VK_NULL_HANDLE
        VkShaderStageFlagBits stages[]={
            VK_SHADER_STAGE_VERTEX_BIT,
            VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
            VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
            VK_SHADER_STAGE_GEOMETRY_BIT,
            VK_SHADER_STAGE_TASK_BIT_EXT,
            VK_SHADER_STAGE_MESH_BIT_EXT,
            VK_SHADER_STAGE_FRAGMENT_BIT};
        VkShaderEXT bindShaders[]={
            VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
            VK_NULL_HANDLE, (VkShaderEXT)meshS, (VkShaderEXT)fragS};
        vkCmdBindShadersEXT(cb,7,stages,bindShaders);
        vkCmdBeginRendering(cb,&ri);
        vkCmdDrawMeshTasksEXT(cb,1,1,1);
        vkCmdEndRendering(cb);
        CHK(vkEndCommandBuffer(cb));

        VkSubmitInfo sb={VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sb.commandBufferCount=1; sb.pCommandBuffers=&cb;
        VkResult sr=vkQueueSubmit(q,1,&sb,fence);
        if(sr==VK_ERROR_DEVICE_LOST){
            fprintf(stderr,"LOST submit f=%d\n",f);
            GFSDK_Aftermath_CrashDump_Status s=GFSDK_Aftermath_CrashDump_Status_Unknown;
            GFSDK_Aftermath_GetCrashDumpStatus(&s);
            for(int w=0;w<200;w++){
                if(s==GFSDK_Aftermath_CrashDump_Status_Finished||s==GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed)break;
                SDL_Delay(50); GFSDK_Aftermath_GetCrashDumpStatus(&s);
            }
            goto end;
        }
        fprintf(stderr,"f%d ok\n",f);
    }

end:
    CHK(vkDeviceWaitIdle(dev));
    if(vkDestroyShaderEXT){
        auto ds=(PFN_vkDestroyShaderEXT)vkDestroyShaderEXT;
        ds(dev,(VkShaderEXT)meshS,nullptr); ds(dev,(VkShaderEXT)fragS,nullptr);
    }
    vkFreeMemory(dev,imgMem,nullptr);
    vkDestroyImageView(dev,tgtView,nullptr); vkDestroyImage(dev,tgt,nullptr);
    vkDestroyFence(dev,fence,nullptr);
    vkDestroyCommandPool(dev,pool,nullptr);
    vkDestroyDevice(dev,nullptr);
    vkDestroySurfaceKHR(inst,surf,nullptr);
    SDL_DestroyWindow(win); SDL_Quit();
    vkDestroyInstance(inst,nullptr);
    fprintf(stderr,"\nDone\n");
    return 0;
}
