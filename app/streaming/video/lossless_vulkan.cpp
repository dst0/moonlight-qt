#include "lossless_vulkan.h"

#ifdef Q_OS_LINUX

#include "lossless_hash.h"
#include "path.h"

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#if __has_include(<vulkan/vulkan.h>)
#define SDS_HAS_VULKAN_HEADERS 1
#include <vulkan/vulkan.h>
#else
#define SDS_HAS_VULKAN_HEADERS 0
#endif

struct LosslessVulkanRenderer::Impl
{
#if SDS_HAS_VULKAN_HEADERS
    void* lib = nullptr;
    PFN_vkGetInstanceProcAddr getInstProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps {};
    std::uint32_t queueFamily = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout plLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    struct HostBuf {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void* mapped = nullptr;
        std::size_t size = 0;
    };
    HostBuf packetBuf, descBuf, prevBuf, currBuf, rowCrcBuf, crcTableBuf;
    int curW = 0, curH = 0;
    bool hasPrev = false;

    PFN_vkCreateInstance pvkCreateInstance = nullptr;
    PFN_vkDestroyInstance pvkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices pvkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties pvkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties pvkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice pvkCreateDevice = nullptr;
    PFN_vkDestroyDevice pvkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue pvkGetDeviceQueue = nullptr;
    PFN_vkCreateDescriptorSetLayout pvkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout pvkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout pvkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout pvkDestroyPipelineLayout = nullptr;
    PFN_vkCreateShaderModule pvkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule pvkDestroyShaderModule = nullptr;
    PFN_vkCreateComputePipelines pvkCreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline pvkDestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool pvkCreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool pvkDestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets pvkAllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets pvkUpdateDescriptorSets = nullptr;
    PFN_vkCreateCommandPool pvkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool pvkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers pvkAllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer pvkResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer pvkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer pvkEndCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline pvkCmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets pvkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants pvkCmdPushConstants = nullptr;
    PFN_vkCmdDispatch pvkCmdDispatch = nullptr;
    PFN_vkCmdPipelineBarrier pvkCmdPipelineBarrier = nullptr;
    PFN_vkCreateFence pvkCreateFence = nullptr;
    PFN_vkDestroyFence pvkDestroyFence = nullptr;
    PFN_vkResetFences pvkResetFences = nullptr;
    PFN_vkWaitForFences pvkWaitForFences = nullptr;
    PFN_vkQueueSubmit pvkQueueSubmit = nullptr;
    PFN_vkCreateBuffer pvkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer pvkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements pvkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory pvkAllocateMemory = nullptr;
    PFN_vkFreeMemory pvkFreeMemory = nullptr;
    PFN_vkBindBufferMemory pvkBindBufferMemory = nullptr;
    PFN_vkMapMemory pvkMapMemory = nullptr;
    PFN_vkUnmapMemory pvkUnmapMemory = nullptr;
    PFN_vkDeviceWaitIdle pvkDeviceWaitIdle = nullptr;

    struct Push {
        std::uint32_t stage, width, height, copy_rect_count, dirty_tile_count, p0, p1, p2;
        std::uint32_t rects[4][4];
    };

    ~Impl()
    {
        if (device != VK_NULL_HANDLE && pvkDeviceWaitIdle) {
            pvkDeviceWaitIdle(device);
            destroyBuf(packetBuf);
            destroyBuf(descBuf);
            destroyBuf(prevBuf);
            destroyBuf(currBuf);
            destroyBuf(rowCrcBuf);
            destroyBuf(crcTableBuf);
            if (fence) pvkDestroyFence(device, fence, nullptr);
            if (cmdPool) pvkDestroyCommandPool(device, cmdPool, nullptr);
            if (descPool) pvkDestroyDescriptorPool(device, descPool, nullptr);
            if (pipeline) pvkDestroyPipeline(device, pipeline, nullptr);
            if (plLayout) pvkDestroyPipelineLayout(device, plLayout, nullptr);
            if (dsl) pvkDestroyDescriptorSetLayout(device, dsl, nullptr);
            pvkDestroyDevice(device, nullptr);
        }
        if (instance && pvkDestroyInstance) pvkDestroyInstance(instance, nullptr);
        if (lib) dlclose(lib);
    }

    bool compileSpirv(std::vector<std::uint32_t>& outSpv)
    {
        QByteArray compSrc = Path::readDataFile("sds_lossless_decode.comp");
        if (compSrc.isEmpty()) {
            QFile rf(QStringLiteral(":/data/sds_lossless_decode.comp"));
            if (rf.open(QIODevice::ReadOnly)) compSrc = rf.readAll();
        }
        if (compSrc.isEmpty()) return false;

        QString compiler = QStandardPaths::findExecutable(QStringLiteral("glslc"));
        bool useGlslang = false;
        if (compiler.isEmpty()) {
            compiler = QStandardPaths::findExecutable(QStringLiteral("glslangValidator"));
            useGlslang = true;
        }
        if (compiler.isEmpty()) return false;

        QTemporaryDir dir;
        if (!dir.isValid()) return false;
        const QString inPath = dir.filePath(QStringLiteral("decode.comp"));
        const QString outPath = dir.filePath(QStringLiteral("decode.spv"));
        QFile inf(inPath);
        if (!inf.open(QIODevice::WriteOnly)) return false;
        inf.write(compSrc);
        inf.close();

        QStringList args = useGlslang
            ? QStringList { QStringLiteral("-V"), QStringLiteral("-o"), outPath, inPath }
            : QStringList { QStringLiteral("-O"), inPath, QStringLiteral("-o"), outPath };
        if (QProcess::execute(compiler, args) != 0) return false;

        QFile spvf(outPath);
        if (!spvf.open(QIODevice::ReadOnly)) return false;
        const QByteArray bytes = spvf.readAll();
        if (bytes.size() < 20 || (bytes.size() % 4) != 0) return false;
        outSpv.resize(static_cast<std::size_t>(bytes.size()) / 4u);
        std::memcpy(outSpv.data(), bytes.constData(), static_cast<std::size_t>(bytes.size()));
        return true;
    }

    bool init()
    {
        std::vector<std::uint32_t> spv;
        if (!compileSpirv(spv)) return false;

        lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        getInstProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
        if (!getInstProc) return false;
        pvkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(getInstProc(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!pvkCreateInstance) return false;

        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "moonlight-lossless", 1, "sds", 1, VK_API_VERSION_1_0 };
        VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr };
        if (pvkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return false;

        auto loadI = [&](const char* n) { return getInstProc(instance, n); };
        pvkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(loadI("vkDestroyInstance"));
        pvkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(loadI("vkEnumeratePhysicalDevices"));
        pvkGetPhysicalDeviceQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(loadI("vkGetPhysicalDeviceQueueFamilyProperties"));
        pvkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(loadI("vkGetPhysicalDeviceMemoryProperties"));
        pvkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(loadI("vkCreateDevice"));
        pvkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(loadI("vkDestroyDevice"));
        pvkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(loadI("vkGetDeviceQueue"));
        pvkCreateDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(loadI("vkCreateDescriptorSetLayout"));
        pvkDestroyDescriptorSetLayout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(loadI("vkDestroyDescriptorSetLayout"));
        pvkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(loadI("vkCreatePipelineLayout"));
        pvkDestroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(loadI("vkDestroyPipelineLayout"));
        pvkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(loadI("vkCreateShaderModule"));
        pvkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(loadI("vkDestroyShaderModule"));
        pvkCreateComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(loadI("vkCreateComputePipelines"));
        pvkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(loadI("vkDestroyPipeline"));
        pvkCreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(loadI("vkCreateDescriptorPool"));
        pvkDestroyDescriptorPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(loadI("vkDestroyDescriptorPool"));
        pvkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(loadI("vkAllocateDescriptorSets"));
        pvkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(loadI("vkUpdateDescriptorSets"));
        pvkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(loadI("vkCreateCommandPool"));
        pvkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(loadI("vkDestroyCommandPool"));
        pvkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(loadI("vkAllocateCommandBuffers"));
        pvkResetCommandBuffer = reinterpret_cast<PFN_vkResetCommandBuffer>(loadI("vkResetCommandBuffer"));
        pvkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(loadI("vkBeginCommandBuffer"));
        pvkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(loadI("vkEndCommandBuffer"));
        pvkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(loadI("vkCmdBindPipeline"));
        pvkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(loadI("vkCmdBindDescriptorSets"));
        pvkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(loadI("vkCmdPushConstants"));
        pvkCmdDispatch = reinterpret_cast<PFN_vkCmdDispatch>(loadI("vkCmdDispatch"));
        pvkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(loadI("vkCmdPipelineBarrier"));
        pvkCreateFence = reinterpret_cast<PFN_vkCreateFence>(loadI("vkCreateFence"));
        pvkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(loadI("vkDestroyFence"));
        pvkResetFences = reinterpret_cast<PFN_vkResetFences>(loadI("vkResetFences"));
        pvkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(loadI("vkWaitForFences"));
        pvkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(loadI("vkQueueSubmit"));
        pvkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(loadI("vkCreateBuffer"));
        pvkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(loadI("vkDestroyBuffer"));
        pvkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(loadI("vkGetBufferMemoryRequirements"));
        pvkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(loadI("vkAllocateMemory"));
        pvkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(loadI("vkFreeMemory"));
        pvkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(loadI("vkBindBufferMemory"));
        pvkMapMemory = reinterpret_cast<PFN_vkMapMemory>(loadI("vkMapMemory"));
        pvkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(loadI("vkUnmapMemory"));
        pvkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(loadI("vkDeviceWaitIdle"));

        std::uint32_t cnt = 0;
        pvkEnumeratePhysicalDevices(instance, &cnt, nullptr);
        if (cnt == 0) return false;
        std::vector<VkPhysicalDevice> pds(cnt);
        pvkEnumeratePhysicalDevices(instance, &cnt, pds.data());
        for (auto pd : pds) {
            std::uint32_t qc = 0;
            pvkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
            std::vector<VkQueueFamilyProperties> qps(qc);
            pvkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qps.data());
            for (std::uint32_t qi = 0; qi < qc; ++qi) {
                if (qps[qi].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                    phys = pd; queueFamily = qi; break;
                }
            }
            if (phys) break;
        }
        if (!phys) return false;
        pvkGetPhysicalDeviceMemoryProperties(phys, &memProps);

        float prio = 1.f;
        VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queueFamily, 1, &prio };
        VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 0, nullptr, nullptr };
        if (pvkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) return false;
        pvkGetDeviceQueue(device, queueFamily, 0, &queue);

        VkDescriptorSetLayoutBinding bindings[6] {};
        for (std::uint32_t i = 0; i < 6; ++i) {
            bindings[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        }
        VkDescriptorSetLayoutCreateInfo dslci { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 6, bindings };
        if (pvkCreateDescriptorSetLayout(device, &dslci, nullptr, &dsl) != VK_SUCCESS) return false;

        VkPushConstantRange pcr { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push) };
        VkPipelineLayoutCreateInfo plci { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pcr };
        if (pvkCreatePipelineLayout(device, &plci, nullptr, &plLayout) != VK_SUCCESS) return false;

        VkShaderModuleCreateInfo smci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spv.size() * 4u, spv.data() };
        VkShaderModule mod = VK_NULL_HANDLE;
        if (pvkCreateShaderModule(device, &smci, nullptr, &mod) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo cpci { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr },
            plLayout, VK_NULL_HANDLE, 0 };
        VkResult pres = pvkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
        pvkDestroyShaderModule(device, mod, nullptr);
        if (pres != VK_SUCCESS) return false;

        VkDescriptorPoolSize psz { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 };
        VkDescriptorPoolCreateInfo dpci { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1, &psz };
        if (pvkCreateDescriptorPool(device, &dpci, nullptr, &descPool) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo dsai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descPool, 1, &dsl };
        if (pvkAllocateDescriptorSets(device, &dsai, &descSet) != VK_SUCCESS) return false;

        VkCommandPoolCreateInfo cpci2 { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queueFamily };
        if (pvkCreateCommandPool(device, &cpci2, nullptr, &cmdPool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo cbai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        if (pvkAllocateCommandBuffers(device, &cbai, &cmdBuf) != VK_SUCCESS) return false;
        VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0 };
        if (pvkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) return false;

        if (!createBuf(256 * sizeof(std::uint32_t), crcTableBuf)) return false;
        auto* tbl = static_cast<std::uint32_t*>(crcTableBuf.mapped);
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? ((c >> 1) ^ 0x82F63B78u) : (c >> 1);
            tbl[i] = c;
        }
        return true;
    }

    bool createBuf(std::size_t sz, HostBuf& out)
    {
        destroyBuf(out);
        VkBufferCreateInfo ci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
        if (pvkCreateBuffer(device, &ci, nullptr, &out.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr {};
        pvkGetBufferMemoryRequirements(device, out.buf, &mr);
        std::uint32_t mid = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (std::uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((mr.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) { mid = i; break; }
        }
        if (mid == UINT32_MAX) { destroyBuf(out); return false; }
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, mr.size, mid };
        if (pvkAllocateMemory(device, &ai, nullptr, &out.mem) != VK_SUCCESS) { destroyBuf(out); return false; }
        pvkBindBufferMemory(device, out.buf, out.mem, 0);
        pvkMapMemory(device, out.mem, 0, sz, 0, &out.mapped);
        out.size = sz;
        return out.mapped != nullptr;
    }

    void destroyBuf(HostBuf& b)
    {
        if (b.mapped) { pvkUnmapMemory(device, b.mem); b.mapped = nullptr; }
        if (b.buf) { pvkDestroyBuffer(device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
        if (b.mem) { pvkFreeMemory(device, b.mem, nullptr); b.mem = VK_NULL_HANDLE; }
        b.size = 0;
    }

    void updateDesc()
    {
        HostBuf* all[6] = { &packetBuf, &descBuf, &prevBuf, &currBuf, &rowCrcBuf, &crcTableBuf };
        VkDescriptorBufferInfo bi[6] {};
        VkWriteDescriptorSet ws[6] {};
        for (int i = 0; i < 6; ++i) {
            bi[i] = { all[i]->buf, 0, all[i]->size };
            ws[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet, static_cast<std::uint32_t>(i), 0, 1,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[i], nullptr };
        }
        pvkUpdateDescriptorSets(device, 6, ws, 0, nullptr);
    }

    bool decode(const std::uint8_t* packet, std::size_t len, const sds::lossless::frame_header_t& hdr,
                bool verifyHash, bool& outHashOk, std::uint64_t& outHashUs)
    {
        sds::lossless::parsed_frame_t parsed;
        if (!sds::lossless::parse_frame_packet(packet, len, parsed)) return false;
        const bool isKey = (hdr.flags & sds::lossless::FLAG_KEYFRAME) != 0;
        const int w = hdr.width, h = hdr.height;
        if (!isKey && (!hasPrev || w != curW || h != curH)) return false;

        std::vector<sds::lossless::gpu_tile_desc_t> descs;
        if (!sds::lossless::build_dirty_tile_descs(parsed, packet, descs)) return false;

        const std::size_t frameBytes = static_cast<std::size_t>(w) * h * 4u;
        const std::size_t pktAlign = (len + 15u) & ~std::size_t(15u);
        const std::size_t descBytes = std::max<std::size_t>(descs.size(), 1) * 32u;
        const std::size_t rowBytes = static_cast<std::size_t>(h) * sizeof(std::uint32_t);
        bool rebind = false;
        if (frameBytes > currBuf.size) { if (!createBuf(frameBytes, prevBuf) || !createBuf(frameBytes, currBuf)) return false; rebind = true; }
        if (pktAlign > packetBuf.size) { if (!createBuf(pktAlign, packetBuf)) return false; rebind = true; }
        if (descBytes > descBuf.size) { if (!createBuf(descBytes, descBuf)) return false; rebind = true; }
        if (rowBytes > rowCrcBuf.size) { if (!createBuf(rowBytes, rowCrcBuf)) return false; rebind = true; }
        if (rebind) updateDesc();

        std::memcpy(packetBuf.mapped, packet, len);
        if (!descs.empty()) {
            std::memcpy(descBuf.mapped, descs.data(), descs.size() * sizeof(sds::lossless::gpu_tile_desc_t));
        }

        Push pc {};
        pc.width = hdr.width; pc.height = hdr.height;
        pc.copy_rect_count = static_cast<std::uint32_t>(std::min<std::size_t>(parsed.copy_rects.size(), sds::lossless::MAX_COPY_RECTS));
        pc.dirty_tile_count = static_cast<std::uint32_t>(descs.size());
        for (std::uint32_t i = 0; i < pc.copy_rect_count; ++i) {
            const auto& r = parsed.copy_rects[i];
            pc.rects[i][0] = static_cast<std::uint32_t>(r.src_x) | (static_cast<std::uint32_t>(r.src_y) << 16);
            pc.rects[i][1] = static_cast<std::uint32_t>(r.dst_x) | (static_cast<std::uint32_t>(r.dst_y) << 16);
            pc.rects[i][2] = static_cast<std::uint32_t>(r.w) | (static_cast<std::uint32_t>(r.h) << 16);
        }

        pvkResetCommandBuffer(cmdBuf, 0);
        VkCommandBufferBeginInfo cbi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };
        pvkBeginCommandBuffer(cmdBuf, &cbi);
        pvkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        pvkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, plLayout, 0, 1, &descSet, 0, nullptr);
        if (!isKey) {
            pc.stage = 0;
            pvkCmdPushConstants(cmdBuf, plLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            pvkCmdDispatch(cmdBuf, static_cast<std::uint32_t>(h), 1, 1);
            VkMemoryBarrier bar { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
            pvkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &bar, 0, nullptr, 0, nullptr);
        }
        if (!descs.empty()) {
            pc.stage = 1;
            pvkCmdPushConstants(cmdBuf, plLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            pvkCmdDispatch(cmdBuf, static_cast<std::uint32_t>(descs.size()), 1, 1);
            VkMemoryBarrier bar { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT };
            pvkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &bar, 0, nullptr, 0, nullptr);
        }
        if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::crc32c) {
            pc.stage = 2;
            pvkCmdPushConstants(cmdBuf, plLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            pvkCmdDispatch(cmdBuf, (static_cast<std::uint32_t>(h) + 31u) / 32u, 1, 1);
        }
        VkMemoryBarrier endBar { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        pvkCmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &endBar, 0, nullptr, 0, nullptr);
        pvkEndCommandBuffer(cmdBuf);

        pvkResetFences(device, 1, &fence);
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmdBuf, 0, nullptr };
        if (pvkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return false;
        if (pvkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;

        QElapsedTimer htimer;
        htimer.start();
        outHashOk = true;
        if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::crc32c) {
            const auto* rowCrcs = static_cast<const std::uint32_t*>(rowCrcBuf.mapped);
            std::uint32_t combined = rowCrcs[0];
            for (int y = 1; y < h; ++y) {
                combined = sds::lossless::crc32c_combine(combined, rowCrcs[y], static_cast<std::uint64_t>(w) * 3ULL);
            }
            const std::uint32_t expected =
                static_cast<std::uint32_t>(hdr.frame_hash[0]) |
                (static_cast<std::uint32_t>(hdr.frame_hash[1]) << 8) |
                (static_cast<std::uint32_t>(hdr.frame_hash[2]) << 16) |
                (static_cast<std::uint32_t>(hdr.frame_hash[3]) << 24);
            outHashOk = (combined == expected);
        } else if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::blake3) {
            const auto b3 = sds::lossless::blake3_rgb24_from_bgra(
                static_cast<const std::uint8_t*>(currBuf.mapped), w * 4, w, h);
            outHashOk = (b3 == hdr.frame_hash);
        }
        outHashUs = static_cast<std::uint64_t>(htimer.nsecsElapsed() / 1000);

        curW = w; curH = h;
        hasPrev = true;
        std::swap(prevBuf, currBuf);
        updateDesc();
        return true;
    }
#endif
};

LosslessVulkanRenderer::~LosslessVulkanRenderer()
{
    delete m_Impl;
    m_Impl = nullptr;
}

bool LosslessVulkanRenderer::initialize(SDL_Window* window,
                                        int width,
                                        int height,
                                        bool enableVsync,
                                        Overlay::OverlayManager* overlayManager,
                                        bool preferGpuCompute)
{
    if (!m_Presenter.initialize(window, width, height, enableVsync, overlayManager, false)) {
        return false;
    }
#if SDS_HAS_VULKAN_HEADERS
    m_Impl = new Impl();
    m_GpuReady = m_Impl->init();
    if (!m_GpuReady) {
        delete m_Impl;
        m_Impl = nullptr;
    }
#endif
    m_UsingGpuCompute = preferGpuCompute && m_GpuReady;
    return true;
}

bool LosslessVulkanRenderer::presentPacket(const std::uint8_t* packet,
                                           std::size_t len,
                                           const sds::lossless::frame_header_t& hdr,
                                           bool useGpuCompute,
                                           bool verifyHash,
                                           bool& outHashOk,
                                           std::uint64_t& outDecodeUs,
                                           std::uint64_t& outHashUs)
{
#if SDS_HAS_VULKAN_HEADERS
    if (useGpuCompute && m_GpuReady && m_Impl) {
        QElapsedTimer timer;
        timer.start();
        m_UsingGpuCompute = true;
        if (m_Impl->decode(packet, len, hdr, verifyHash, outHashOk, outHashUs)) {
            outDecodeUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
            // note: decode swapped prevBuf <-> currBuf; prevBuf holds newly decoded frame
            bool dummyHashOk = true;
            std::uint64_t dummyUs = 0;
            return m_Presenter.presentRawBgra(
                static_cast<const std::uint8_t*>(m_Impl->prevBuf.mapped),
                hdr.width * 4,
                hdr.width,
                hdr.height,
                hdr,
                false,
                dummyHashOk,
                dummyUs);
        }
    }
#endif
    if (m_Impl) {
        m_Impl->hasPrev = false;
    }
    m_UsingGpuCompute = false;
    return m_Presenter.presentPacket(packet, len, hdr, false, verifyHash, outHashOk, outDecodeUs, outHashUs);
}

void LosslessVulkanRenderer::renderOnMainThread()
{
    m_Presenter.renderOnMainThread();
}

bool LosslessVulkanRenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    return m_Presenter.notifyWindowChanged(info);
}

void LosslessVulkanRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    m_Presenter.notifyOverlayUpdated(type);
}

#endif
