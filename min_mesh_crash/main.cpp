#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
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
    auto e=f.tellg(); f.seekg(0);
    std::vector<uint8_t> b(e); f.read((char*)b.data(),e); return b;
}

    using ShaderHandle=void*;

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
    // Get required Vulkan surface extensions from SDL3
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
        // Pick NVIDIA (0x10de) if available, else first discrete
        if(p.vendorID==0x10de){pd=pds[i];break;}
    }
    if(!pd){pd=pds[0];}
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd,&pp);
    fprintf(stderr,"Selected: %s\n",pp.deviceName);

    const char* de[]={
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
        VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
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
        fprintf(stderr,"WARN: selected GPU cannot present! Will crash on present.\n");
        fprintf(stderr,"Running without swapchain to isolate mesh shader crash...\n");
    }

    float pq=1;
    VkDeviceQueueCreateInfo dq={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex=gq; dq.queueCount=1; dq.pQueuePriorities=&pq;

    VkDeviceDiagnosticsConfigCreateInfoNV dgc={VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV};
    dgc.flags=VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV
            |VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV
            |VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV
            |VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV;

    VkPhysicalDeviceMeshShaderFeaturesEXT mf={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    mf.meshShader=VK_TRUE;
    VkPhysicalDeviceDescriptorHeapFeaturesEXT hf={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    hf.descriptorHeap=VK_TRUE;

    VkPhysicalDeviceFeatures2 f2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    dgc.pNext=&f2; f2.pNext=&mf; mf.pNext=&hf;

    VkDeviceCreateInfo dc2={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc2.pNext=&dgc; dc2.queueCreateInfoCount=1; dc2.pQueueCreateInfos=&dq;
    dc2.enabledExtensionCount=sizeof(de)/sizeof(*de); dc2.ppEnabledExtensionNames=de;
    VkDevice dev; CHK(vkCreateDevice(pd,&dc2,nullptr,&dev));

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
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=tgt; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=fmt;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView tgtView; CHK(vkCreateImageView(dev,&vi,nullptr,&tgtView));
    fprintf(stderr,"view created\n");

    // VMA + heap
    VmaAllocator vma; VmaAllocatorCreateInfo vc={};
    vc.flags=VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vc.physicalDevice=pd; vc.device=dev; vc.instance=inst; vc.vulkanApiVersion=VK_API_VERSION_1_3;
    CHK(vmaCreateAllocator(&vc,&vma));
    fprintf(stderr,"vma allocator created\n");

    uint32_t dsz=(uint32_t)hp.bufferDescriptorSize;
    uint32_t res=(uint32_t)(hp.minResourceHeapReservedRange+dsz-1)/dsz;
    uint32_t cap=16+res;
    VkBufferCreateInfo bc={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size=dsz*cap; bc.usage=VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT;
    fprintf(stderr,"trying vkCreateBuffer size=%u\n",(unsigned)bc.size);
    VkBuffer hbuf;
    VkResult _hr2=vkCreateBuffer(dev,&bc,nullptr,&hbuf);
    fprintf(stderr,"vkCreateBuffer returned %d\n",(int)_hr2);
    if(_hr2!=VK_SUCCESS){fprintf(stderr,"vkCreateBuffer failed %d\n",(int)_hr2);return 1;}
    fprintf(stderr,"vkCreateBuffer ok\n");

    // Find memory type
    VkMemoryRequirements reqs; vkGetBufferMemoryRequirements(dev,hbuf,&reqs);
    fprintf(stderr,"mem reqs size=%zu typeBits=0x%x\n",reqs.size,reqs.memoryTypeBits);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp);
    uint32_t mt=0;
    for(uint32_t i=0;i<mp.memoryTypeCount;i++){
        if((reqs.memoryTypeBits&(1u<<i))&&(mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){
            mt=i; break;
        }
    }
    fprintf(stderr,"mem type=%u\n",mt);

    VkMemoryAllocateInfo mai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize=reqs.size; mai.memoryTypeIndex=mt;
    VkDeviceAddress haddr=0;
    // Request device address
    VkMemoryAllocateFlagsInfo maf={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    maf.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    mai.pNext=&maf;
    VkDeviceMemory hmem;
    VkResult _hr3=vkAllocateMemory(dev,&mai,nullptr,&hmem);
    fprintf(stderr,"vkAllocateMemory returned %d\n",(int)_hr3);
    if(_hr3!=VK_SUCCESS){fprintf(stderr,"vkAllocateMemory failed %d\n",(int)_hr3);return 1;}
    fprintf(stderr,"vkAllocateMemory ok\n");

    CHK(vkBindBufferMemory(dev,hbuf,hmem,0));
    VkBufferDeviceAddressInfo bai={VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bai.buffer=hbuf; haddr=vkGetBufferDeviceAddress(dev,&bai);
    fprintf(stderr,"heap: addr=0x%lx\n",(unsigned long)haddr);

    void* mapped;
    CHK(vkMapMemory(dev,hmem,0,VK_WHOLE_SIZE,0,&mapped));
    memset(mapped,0,reqs.size);
    fprintf(stderr,"heap mapped ok\n");

    fprintf(stderr,"shaders loading\n");
    auto mspv=readBin("mesh.spv"); auto fspv=readBin("frag.spv");
    fprintf(stderr,"shaders loaded mesh=%zu frag=%zu\n",mspv.size(),fspv.size());

    // vkCreateShadersEXT
    PFN_vkCreateShadersEXT vkCreateShadersEXT=(PFN_vkCreateShadersEXT)vkGetDeviceProcAddr(dev,"vkCreateShadersEXT");
    PFN_vkDestroyShaderEXT vkDestroyShaderEXT=(PFN_vkDestroyShaderEXT)vkGetDeviceProcAddr(dev,"vkDestroyShaderEXT");
    PFN_vkCmdBindShadersEXT vkCmdBindShadersEXT=(PFN_vkCmdBindShadersEXT)vkGetDeviceProcAddr(dev,"vkCmdBindShadersEXT");
    PFN_vkCmdBindResourceHeapEXT vkCmdBindResourceHeapEXT=(PFN_vkCmdBindResourceHeapEXT)vkGetDeviceProcAddr(dev,"vkCmdBindResourceHeapEXT");
    PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT=(PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(dev,"vkCmdDrawMeshTasksEXT");
    if(!vkCreateShadersEXT||!vkCmdBindShadersEXT||!vkCmdBindResourceHeapEXT||!vkCmdDrawMeshTasksEXT){
        fprintf(stderr,"ext funcs missing\n"); return 1;
    }

    VkShaderCreateInfoEXT meshCI={VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    meshCI.stage=VK_SHADER_STAGE_MESH_BIT_EXT;
    meshCI.codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT;
    meshCI.codeSize=mspv.size();
    meshCI.pCode=(const uint32_t*)mspv.data();
    meshCI.pName=(char*)"main";
    ShaderHandle meshS;
    CHK(((PFN_vkCreateShadersEXT)vkCreateShadersEXT)(dev,1,&meshCI,nullptr,(void**)&meshS));
    fprintf(stderr,"mesh shader created\n");

    VkShaderCreateInfoEXT fragCI={VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    fragCI.stage=VK_SHADER_STAGE_FRAGMENT_BIT;
    fragCI.codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT;
    fragCI.codeSize=fspv.size();
    fragCI.pCode=(const uint32_t*)fspv.data();
    fragCI.pName=(char*)"main";
    ShaderHandle fragS;
    CHK(((PFN_vkCreateShadersEXT)vkCreateShadersEXT)(dev,1,&fragCI,nullptr,(void**)&fragS));

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

    VkSemaphoreCreateInfo si={VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore imgS,renS; CHK(vkCreateSemaphore(dev,&si,nullptr,&imgS));
    CHK(vkCreateSemaphore(dev,&si,nullptr,&renS));
    VkFenceCreateInfo fi={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence; CHK(vkCreateFence(dev,&fi,nullptr,&fence));

    for(int f=0;f<10;f++){
        fprintf(stderr,"frame %d start\n",f);
        VkImageLayout oldL=f?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        CHK(vkResetFences(dev,1,&fence));
        CHK(vkResetCommandBuffer(cb,0));
        VkCommandBufferBeginInfo cbb={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHK(vkBeginCommandBuffer(cb,&cbb));

        // Image barrier: undefined -> color attachment
        VkImageMemoryBarrier2 bar={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        bar.image=tgt; bar.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        bar.srcQueueFamilyIndex=bar.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        bar.oldLayout=oldL; bar.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.srcAccessMask=0; bar.dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        bar.srcStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        bar.dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkDependencyInfo di={VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount=1; di.pImageMemoryBarriers=&bar;
        vkCmdPipelineBarrier2(cb,&di);

        // heap
        VkMemoryBarrier2 hb={VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        hb.srcAccessMask=VK_ACCESS_2_HOST_WRITE_BIT; hb.dstAccessMask=VK_ACCESS_2_RESOURCE_HEAP_READ_BIT_EXT;
        hb.srcStageMask=VK_PIPELINE_STAGE_2_HOST_BIT; hb.dstStageMask=VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        VkDependencyInfo hd={VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        hd.memoryBarrierCount=1; hd.pMemoryBarriers=&hb;
        vkCmdPipelineBarrier2(cb,&hd);

        VkDeviceAddressRangeEXT hr={}; hr.address=haddr; hr.size=reqs.size;
        VkBindHeapInfoEXT bi={VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT};
        bi.heapRange=hr; bi.reservedRangeOffset=0; bi.reservedRangeSize=hp.minResourceHeapReservedRange;
        vkCmdBindResourceHeapEXT(cb,&bi);

        VkRenderingAttachmentInfo ca={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView=tgtView; ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color.float32[0]=0.1f;
        VkRenderingInfo ri={VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea={{0,0},ex}; ri.layerCount=1;
        ri.colorAttachmentCount=1; ri.pColorAttachments=&ca;

        VkShaderStageFlagBits stages[]={VK_SHADER_STAGE_MESH_BIT_EXT,VK_SHADER_STAGE_FRAGMENT_BIT};
        ShaderHandle shaders[]={meshS,fragS};
        vkCmdBindShadersEXT(cb,2,stages,(const void**)shaders);
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
        ds(dev,meshS,nullptr); ds(dev,fragS,nullptr);
    }
    vkUnmapMemory(dev,hmem); vkDestroyBuffer(dev,hbuf,nullptr); vkFreeMemory(dev,hmem,nullptr);
    vkDestroyImageView(dev,tgtView,nullptr); vkDestroyImage(dev,tgt,nullptr);
    vkDestroyFence(dev,fence,nullptr); vkDestroySemaphore(dev,renS,nullptr); vkDestroySemaphore(dev,imgS,nullptr);
    vkDestroyCommandPool(dev,pool,nullptr);
    vkDestroyDevice(dev,nullptr);
    vkDestroySurfaceKHR(inst,surf,nullptr);
    SDL_DestroyWindow(win); SDL_Quit();
    vkDestroyInstance(inst,nullptr);
    fprintf(stderr,"\nDone\n");
    return 0;
}
