#include "ssr_vulkan_harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(SAIGE_SSR_ENABLE_VULKAN) && SAIGE_SSR_ENABLE_VULKAN
#include <vulkan/vulkan.h>
#include <fstream>
#endif

namespace saige::ssr::gpu {

namespace {

[[nodiscard]] [[maybe_unused]] ColorRgb tonemap_srgb(ColorRgb hdr) noexcept {
  auto tm = [](float v) -> float {
    const float mapped = v / (1.0f + std::max(0.0f, v));
    return std::pow(std::clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f);
  };
  return {tm(hdr.r), tm(hdr.g), tm(hdr.b)};
}

[[maybe_unused]] void compute_parity_metrics(const std::vector<ColorRgb>& cpu_ref,
                            const std::vector<ColorRgb>& gpu_out,
                            VulkanParityReport& report) noexcept {
  if (cpu_ref.empty() || cpu_ref.size() != gpu_out.size()) return;
  double mse = 0.0;
  double mae = 0.0;
  double max_err = 0.0;
  for (std::size_t i = 0; i < cpu_ref.size(); ++i) {
    const ColorRgb a = tonemap_srgb(cpu_ref[i]);
    const ColorRgb b = tonemap_srgb(gpu_out[i]);
    const double dr = std::abs(static_cast<double>(a.r - b.r));
    const double dg = std::abs(static_cast<double>(a.g - b.g));
    const double db = std::abs(static_cast<double>(a.b - b.b));
    mae += (dr + dg + db) / 3.0;
    mse += (dr * dr + dg * dg + db * db) / 3.0;
    max_err = std::max({max_err, dr, dg, db});
  }
  const double n = static_cast<double>(cpu_ref.size());
  mae /= n;
  mse /= n;
  report.cpu_vs_gpu_mae = mae;
  report.cpu_vs_gpu_max_abs_err = max_err;
  report.cpu_vs_gpu_psnr_db = (mse <= 1e-12) ? 99.0 : (10.0 * std::log10(1.0 / mse));
}

#if defined(SAIGE_SSR_ENABLE_VULKAN) && SAIGE_SSR_ENABLE_VULKAN

float f16_to_f32(std::uint16_t h) noexcept {
  const std::uint32_t sign = (static_cast<std::uint32_t>(h) & 0x8000u) << 16;
  const std::uint32_t exp = (static_cast<std::uint32_t>(h) & 0x7C00u) >> 10;
  const std::uint32_t mant = static_cast<std::uint32_t>(h) & 0x03FFu;
  std::uint32_t bits = 0;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      std::uint32_t m = mant;
      std::uint32_t e = 127 - 14;
      while ((m & 0x0400u) == 0) {
        m <<= 1;
        --e;
      }
      m &= 0x03FFu;
      bits = sign | (e << 23) | (m << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
  }
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(float));
  return out;
}

std::vector<std::uint32_t> load_spv(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) return {};
  const auto sz = static_cast<std::size_t>(f.tellg());
  if (sz == 0 || (sz % 4) != 0) return {};
  std::vector<std::uint32_t> words(sz / 4);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(sz));
  return words;
}

struct ReconstructPushConstants {
  std::uint32_t render_w, render_h;
  std::uint32_t output_w, output_h;
  float jitter_x, jitter_y;
  float exposure_ratio;
  float disocclusion_threshold;
  std::uint32_t flags;
};

struct SharpenPushConstants {
  std::uint32_t output_w, output_h;
  float sharpness;
};

struct GpuImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

struct GpuBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT*,
    void* user_data) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && user_data) {
    auto* clean_flag = static_cast<bool*>(user_data);
    *clean_flag = false;
  }
  return VK_FALSE;
}

#endif  // SAIGE_SSR_ENABLE_VULKAN

}  // namespace

struct SsrVulkanHarness::Impl {
#if defined(SAIGE_SSR_ENABLE_VULKAN) && SAIGE_SSR_ENABLE_VULKAN
  VulkanHarnessConfig cfg{};
  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
  bool validation_clean = true;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  std::uint32_t compute_queue_family = 0;
  VkQueue compute_queue = VK_NULL_HANDLE;
  float timestamp_period_ns = 1.0f;
  std::string device_name;
  bool is_software_device = false;
  bool fp16_active = false;

  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd_buf = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSampler linear_sampler = VK_NULL_HANDLE;
  VkQueryPool query_pool = VK_NULL_HANDLE;

  VkDescriptorSetLayout recon_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout recon_pipe_layout = VK_NULL_HANDLE;
  VkPipeline recon_pipeline = VK_NULL_HANDLE;

  VkDescriptorSetLayout sharp_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout sharp_pipe_layout = VK_NULL_HANDLE;
  VkPipeline sharp_pipeline = VK_NULL_HANDLE;

  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkDescriptorSet recon_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDescriptorSet sharp_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

  GpuImage color_hdr{};
  GpuImage depth{};
  GpuImage motion_px{};
  GpuImage reactive_mask{};
  GpuImage transparency_mask{};

  GpuImage hist_color[2]{};
  GpuImage hist_depth[2]{};
  GpuImage hist_conf[2]{};
  GpuImage final_output{};

  GpuBuffer upload_staging{};
  GpuBuffer readback_staging{};

  std::uint32_t alloc_ren_w = 0, alloc_ren_h = 0;
  std::uint32_t alloc_out_w = 0, alloc_out_h = 0;
  int ping_pong = 0;
  bool has_history = false;
  float prev_exposure = 1.0f;
  bool initialized = false;

  std::uint32_t find_memory_type(std::uint32_t type_filter,
                                 VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((type_filter & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & props) == props) {
        return i;
      }
    }
    return UINT32_MAX;
  }

  bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags props, GpuBuffer& out) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bci, nullptr, &out.buffer) != VK_SUCCESS)
      return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    const std::uint32_t mt = find_memory_type(req.memoryTypeBits, props);
    if (mt == UINT32_MAX) return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS)
      return false;
    if (vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS)
      return false;
    out.size = size;
    return true;
  }

  void destroy_buffer(GpuBuffer& b) {
    if (b.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory != VK_NULL_HANDLE) vkFreeMemory(device, b.memory, nullptr);
    b = {};
  }

  bool create_image(std::uint32_t w, std::uint32_t h, VkFormat fmt,
                    VkImageUsageFlags usage, GpuImage& out) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ici, nullptr, &out.image) != VK_SUCCESS)
      return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, out.image, &req);
    const std::uint32_t mt =
        find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS)
      return false;
    if (vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS)
      return false;

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = out.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = fmt;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &ivci, nullptr, &out.view) != VK_SUCCESS)
      return false;

    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.width = w;
    out.height = h;
    out.format = fmt;
    return true;
  }

  void destroy_image(GpuImage& im) {
    if (im.view != VK_NULL_HANDLE) vkDestroyImageView(device, im.view, nullptr);
    if (im.image != VK_NULL_HANDLE) vkDestroyImage(device, im.image, nullptr);
    if (im.memory != VK_NULL_HANDLE) vkFreeMemory(device, im.memory, nullptr);
    im = {};
  }

  void transition_image(VkCommandBuffer cmd, GpuImage& im,
                        VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                        VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                        VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = im.layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    im.layout = new_layout;
  }

  void update_descriptors() {
    for (int pp = 0; pp < 2; ++pp) {
      const int in_idx = pp;
      const int out_idx = 1 - pp;

      VkDescriptorImageInfo sampled[8] = {
          {linear_sampler, color_hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {linear_sampler, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {linear_sampler, motion_px.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {linear_sampler, reactive_mask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {linear_sampler, transparency_mask.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {linear_sampler, hist_color[in_idx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {linear_sampler, hist_depth[in_idx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
          {linear_sampler, hist_conf[in_idx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      };
      VkDescriptorImageInfo storage[3] = {
          {VK_NULL_HANDLE, hist_color[out_idx].view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, hist_depth[out_idx].view, VK_IMAGE_LAYOUT_GENERAL},
          {VK_NULL_HANDLE, hist_conf[out_idx].view, VK_IMAGE_LAYOUT_GENERAL},
      };
      VkWriteDescriptorSet writes[11]{};
      for (std::uint32_t i = 0; i < 8; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = recon_sets[pp];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &sampled[i];
      }
      for (std::uint32_t i = 0; i < 3; ++i) {
        writes[8 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8 + i].dstSet = recon_sets[pp];
        writes[8 + i].dstBinding = 8 + i;
        writes[8 + i].descriptorCount = 1;
        writes[8 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[8 + i].pImageInfo = &storage[i];
      }
      vkUpdateDescriptorSets(device, 11, writes, 0, nullptr);

      VkDescriptorImageInfo s_in{linear_sampler, hist_color[out_idx].view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo s_out{VK_NULL_HANDLE, final_output.view,
                                  VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet sw[2]{};
      sw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      sw[0].dstSet = sharp_sets[pp];
      sw[0].dstBinding = 0;
      sw[0].descriptorCount = 1;
      sw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      sw[0].pImageInfo = &s_in;
      sw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      sw[1].dstSet = sharp_sets[pp];
      sw[1].dstBinding = 1;
      sw[1].descriptorCount = 1;
      sw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      sw[1].pImageInfo = &s_out;
      vkUpdateDescriptorSets(device, 2, sw, 0, nullptr);
    }
  }
#endif
};

SsrVulkanHarness::~SsrVulkanHarness() { destroy(); }

void SsrVulkanHarness::destroy() noexcept {
  if (!impl_) return;
#if defined(SAIGE_SSR_ENABLE_VULKAN) && SAIGE_SSR_ENABLE_VULKAN
  if (impl_->device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(impl_->device);
    impl_->destroy_buffer(impl_->upload_staging);
    impl_->destroy_buffer(impl_->readback_staging);
    impl_->destroy_image(impl_->color_hdr);
    impl_->destroy_image(impl_->depth);
    impl_->destroy_image(impl_->motion_px);
    impl_->destroy_image(impl_->reactive_mask);
    impl_->destroy_image(impl_->transparency_mask);
    for (int i = 0; i < 2; ++i) {
      impl_->destroy_image(impl_->hist_color[i]);
      impl_->destroy_image(impl_->hist_depth[i]);
      impl_->destroy_image(impl_->hist_conf[i]);
    }
    impl_->destroy_image(impl_->final_output);
    if (impl_->desc_pool) vkDestroyDescriptorPool(impl_->device, impl_->desc_pool, nullptr);
    if (impl_->sharp_pipeline) vkDestroyPipeline(impl_->device, impl_->sharp_pipeline, nullptr);
    if (impl_->sharp_pipe_layout) vkDestroyPipelineLayout(impl_->device, impl_->sharp_pipe_layout, nullptr);
    if (impl_->sharp_set_layout) vkDestroyDescriptorSetLayout(impl_->device, impl_->sharp_set_layout, nullptr);
    if (impl_->recon_pipeline) vkDestroyPipeline(impl_->device, impl_->recon_pipeline, nullptr);
    if (impl_->recon_pipe_layout) vkDestroyPipelineLayout(impl_->device, impl_->recon_pipe_layout, nullptr);
    if (impl_->recon_set_layout) vkDestroyDescriptorSetLayout(impl_->device, impl_->recon_set_layout, nullptr);
    if (impl_->query_pool) vkDestroyQueryPool(impl_->device, impl_->query_pool, nullptr);
    if (impl_->linear_sampler) vkDestroySampler(impl_->device, impl_->linear_sampler, nullptr);
    if (impl_->fence) vkDestroyFence(impl_->device, impl_->fence, nullptr);
    if (impl_->cmd_pool) vkDestroyCommandPool(impl_->device, impl_->cmd_pool, nullptr);
    vkDestroyDevice(impl_->device, nullptr);
  }
  if (impl_->messenger && impl_->instance) {
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(impl_->instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn) fn(impl_->instance, impl_->messenger, nullptr);
  }
  if (impl_->instance) vkDestroyInstance(impl_->instance, nullptr);
#endif
  delete impl_;
  impl_ = nullptr;
}

void SsrVulkanHarness::reset_history() noexcept {
#if defined(SAIGE_SSR_ENABLE_VULKAN) && SAIGE_SSR_ENABLE_VULKAN
  if (impl_) {
    impl_->has_history = false;
    impl_->prev_exposure = 1.0f;
  }
#endif
}

bool SsrVulkanHarness::initialize(const VulkanHarnessConfig& cfg,
                                  std::string& out_status) {
  destroy();
#if !defined(SAIGE_SSR_ENABLE_VULKAN) || !SAIGE_SSR_ENABLE_VULKAN
  (void)cfg;
  out_status =
      "BLOCKED: Compiled without SAIGE_SSR_ENABLE_VULKAN=1 "
      "(vulkan.h / glslangValidator unavailable in build environment).";
  return false;
#else
  impl_ = new Impl();
  impl_->cfg = cfg;

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = "SAIGE_SSR_Harness";
  app_info.apiVersion = VK_API_VERSION_1_4;

  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
  const char* exts[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app_info;
  if (cfg.enable_validation_layers) {
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = exts;
  }
  if (vkCreateInstance(&ici, nullptr, &impl_->instance) != VK_SUCCESS) {
    out_status = "vkCreateInstance failed.";
    destroy();
    return false;
  }

  if (cfg.enable_validation_layers) {
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(impl_->instance, "vkCreateDebugUtilsMessengerEXT"));
    if (fn) {
      VkDebugUtilsMessengerCreateInfoEXT dci{};
      dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      dci.pfnUserCallback = debug_callback;
      dci.pUserData = &impl_->validation_clean;
      fn(impl_->instance, &dci, nullptr, &impl_->messenger);
    }
  }

  std::uint32_t dev_count = 0;
  vkEnumeratePhysicalDevices(impl_->instance, &dev_count, nullptr);
  if (dev_count == 0) {
    out_status = "No Vulkan physical devices available.";
    destroy();
    return false;
  }
  std::vector<VkPhysicalDevice> devs(dev_count);
  vkEnumeratePhysicalDevices(impl_->instance, &dev_count, devs.data());

  impl_->physical_device = devs[0];
  for (VkPhysicalDevice d : devs) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(d, &props);
    const bool is_cpu = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
    if ((cfg.prefer_software_vulkan && is_cpu) ||
        (!cfg.prefer_software_vulkan && !is_cpu)) {
      impl_->physical_device = d;
      break;
    }
  }

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(impl_->physical_device, &props);
  impl_->device_name = props.deviceName;
  impl_->is_software_device = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
  impl_->timestamp_period_ns = props.limits.timestampPeriod;

  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  VkPhysicalDeviceVulkan12Features f12{};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  f12.pNext = &f13;
  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f2.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(impl_->physical_device, &f2);

  std::vector<std::uint32_t> recon_spv;
  if (cfg.prefer_fp16 && f12.shaderFloat16 == VK_TRUE) {
    recon_spv = load_spv(cfg.reconstruct_fp16_spv_path);
    impl_->fp16_active = !recon_spv.empty();
  }
  if (recon_spv.empty()) {
    recon_spv = load_spv(cfg.reconstruct_spv_path);
    impl_->fp16_active = false;
  }
  const auto sharp_spv = load_spv(cfg.sharpen_spv_path);
  if (recon_spv.empty() || sharp_spv.empty()) {
    out_status = "Missing compiled SPIR-V binaries (.spv); compile GLSL shaders first.";
    destroy();
    return false;
  }

  std::uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(impl_->physical_device, &qf_count, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qf_count);
  vkGetPhysicalDeviceQueueFamilyProperties(impl_->physical_device, &qf_count, qfs.data());
  bool found_qf = false;
  for (std::uint32_t i = 0; i < qf_count; ++i) {
    if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      impl_->compute_queue_family = i;
      found_qf = true;
      break;
    }
  }
  if (!found_qf) {
    out_status = "No Vulkan compute queue family found.";
    destroy();
    return false;
  }

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = impl_->compute_queue_family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  VkPhysicalDeviceVulkan13Features en13{};
  en13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  en13.synchronization2 = VK_TRUE;
  VkPhysicalDeviceVulkan12Features en12{};
  en12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  en12.pNext = &en13;
  en12.shaderFloat16 = impl_->fp16_active ? VK_TRUE : VK_FALSE;

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.pNext = &en12;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  if (vkCreateDevice(impl_->physical_device, &dci, nullptr, &impl_->device) != VK_SUCCESS) {
    out_status = "vkCreateDevice failed.";
    destroy();
    return false;
  }
  vkGetDeviceQueue(impl_->device, impl_->compute_queue_family, 0, &impl_->compute_queue);

  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = impl_->compute_queue_family;
  if (vkCreateCommandPool(impl_->device, &cpci, nullptr, &impl_->cmd_pool) != VK_SUCCESS) {
    out_status = "vkCreateCommandPool failed.";
    destroy();
    return false;
  }

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = impl_->cmd_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  vkAllocateCommandBuffers(impl_->device, &cbai, &impl_->cmd_buf);

  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  vkCreateFence(impl_->device, &fci, nullptr, &impl_->fence);

  VkSamplerCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(impl_->device, &sci, nullptr, &impl_->linear_sampler);

  VkQueryPoolCreateInfo qpci{};
  qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpci.queryCount = 4;
  vkCreateQueryPool(impl_->device, &qpci, nullptr, &impl_->query_pool);

  // Descriptor set layout for reconstruct (8 sampled + 3 storage)
  VkDescriptorSetLayoutBinding r_binds[11]{};
  for (std::uint32_t i = 0; i < 8; ++i) {
    r_binds[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                  VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  for (std::uint32_t i = 0; i < 3; ++i) {
    r_binds[8 + i] = {8 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                      VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  VkDescriptorSetLayoutCreateInfo r_dsl{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 11, r_binds};
  vkCreateDescriptorSetLayout(impl_->device, &r_dsl, nullptr, &impl_->recon_set_layout);

  VkPushConstantRange r_pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof(ReconstructPushConstants)};
  VkPipelineLayoutCreateInfo r_pli{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1,
      &impl_->recon_set_layout, 1, &r_pcr};
  vkCreatePipelineLayout(impl_->device, &r_pli, nullptr, &impl_->recon_pipe_layout);

  // Descriptor set layout for sharpen (1 sampled + 1 storage)
  VkDescriptorSetLayoutBinding s_binds[2] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  VkDescriptorSetLayoutCreateInfo s_dsl{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, s_binds};
  vkCreateDescriptorSetLayout(impl_->device, &s_dsl, nullptr, &impl_->sharp_set_layout);

  VkPushConstantRange s_pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof(SharpenPushConstants)};
  VkPipelineLayoutCreateInfo s_pli{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1,
      &impl_->sharp_set_layout, 1, &s_pcr};
  vkCreatePipelineLayout(impl_->device, &s_pli, nullptr, &impl_->sharp_pipe_layout);

  // Compute pipelines
  auto create_comp_pipe = [&](const std::vector<std::uint32_t>& code,
                              VkPipelineLayout layout, VkPipeline& out_pipe) {
    VkShaderModuleCreateInfo smci{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
        code.size() * sizeof(std::uint32_t), code.data()};
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(impl_->device, &smci, nullptr, &mod) != VK_SUCCESS)
      return false;
    VkComputePipelineCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    cp.layout = layout;
    const VkResult r = vkCreateComputePipelines(impl_->device, VK_NULL_HANDLE,
                                                1, &cp, nullptr, &out_pipe);
    vkDestroyShaderModule(impl_->device, mod, nullptr);
    return r == VK_SUCCESS;
  };
  if (!create_comp_pipe(recon_spv, impl_->recon_pipe_layout, impl_->recon_pipeline) ||
      !create_comp_pipe(sharp_spv, impl_->sharp_pipe_layout, impl_->sharp_pipeline)) {
    out_status = "vkCreateComputePipelines failed.";
    destroy();
    return false;
  }

  VkDescriptorPoolSize pool_sizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 18},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
  };
  VkDescriptorPoolCreateInfo dpci{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 4, 2, pool_sizes};
  vkCreateDescriptorPool(impl_->device, &dpci, nullptr, &impl_->desc_pool);

  VkDescriptorSetLayout set_layouts[4] = {
      impl_->recon_set_layout, impl_->recon_set_layout,
      impl_->sharp_set_layout, impl_->sharp_set_layout};
  VkDescriptorSet sets[4]{};
  VkDescriptorSetAllocateInfo dsai{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
      impl_->desc_pool, 4, set_layouts};
  vkAllocateDescriptorSets(impl_->device, &dsai, sets);
  impl_->recon_sets[0] = sets[0];
  impl_->recon_sets[1] = sets[1];
  impl_->sharp_sets[0] = sets[2];
  impl_->sharp_sets[1] = sets[3];

  impl_->initialized = true;
  out_status = "Initialized Vulkan 1.4 SSR harness on " + impl_->device_name;
  return true;
#endif
}

VulkanParityReport SsrVulkanHarness::execute_frame(
    const SsrDispatchInputs& inputs,
    const std::vector<ColorRgb>& cpu_reference_output,
    std::vector<ColorRgb>& out_gpu_color) {
  VulkanParityReport report{};
#if !defined(SAIGE_SSR_ENABLE_VULKAN) || !SAIGE_SSR_ENABLE_VULKAN
  (void)inputs;
  (void)cpu_reference_output;
  (void)out_gpu_color;
  report.executed = false;
  report.status_message =
      "BLOCKED: Compiled without SAIGE_SSR_ENABLE_VULKAN=1 "
      "(vulkan.h / glslangValidator unavailable in build environment).";
  return report;
#else
  if (!impl_ || !impl_->initialized) {
    report.status_message = "Harness not initialized.";
    return report;
  }
  report.device_name = impl_->device_name;
  report.is_software_device = impl_->is_software_device;
  report.fp16_active = impl_->fp16_active;

  if (inputs.render_width == 0 || inputs.render_height == 0 ||
      inputs.output_width == 0 || inputs.output_height == 0 ||
      inputs.render_width > inputs.output_width ||
      inputs.render_height > inputs.output_height ||
      !inputs.color_hdr || !inputs.depth || !inputs.motion_vectors_px) {
    report.status_message = "Invalid SSR dispatch inputs.";
    return report;
  }

  const std::uint32_t rw = inputs.render_width;
  const std::uint32_t rh = inputs.render_height;
  const std::uint32_t ow = inputs.output_width;
  const std::uint32_t oh = inputs.output_height;
  const std::size_t ren_n = static_cast<std::size_t>(rw) * rh;
  const std::size_t out_n = static_cast<std::size_t>(ow) * oh;

  bool need_desc_update = false;
  if (rw != impl_->alloc_ren_w || rh != impl_->alloc_ren_h) {
    impl_->destroy_image(impl_->color_hdr);
    impl_->destroy_image(impl_->depth);
    impl_->destroy_image(impl_->motion_px);
    impl_->destroy_image(impl_->reactive_mask);
    impl_->destroy_image(impl_->transparency_mask);
    impl_->destroy_buffer(impl_->upload_staging);

    const VkImageUsageFlags in_usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    impl_->create_image(rw, rh, VK_FORMAT_R32G32B32A32_SFLOAT, in_usage, impl_->color_hdr);
    impl_->create_image(rw, rh, VK_FORMAT_R32_SFLOAT, in_usage, impl_->depth);
    impl_->create_image(rw, rh, VK_FORMAT_R32G32_SFLOAT, in_usage, impl_->motion_px);
    impl_->create_image(rw, rh, VK_FORMAT_R32_SFLOAT, in_usage, impl_->reactive_mask);
    impl_->create_image(rw, rh, VK_FORMAT_R32_SFLOAT, in_usage, impl_->transparency_mask);

    // Total staging bytes: rgba32f(16) + r32f(4) + rg32f(8) + r32f(4) + r32f(4) = 36 bytes/px
    impl_->create_buffer(ren_n * 36ull, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         impl_->upload_staging);
    impl_->alloc_ren_w = rw;
    impl_->alloc_ren_h = rh;
    need_desc_update = true;
  }

  if (ow != impl_->alloc_out_w || oh != impl_->alloc_out_h) {
    for (int i = 0; i < 2; ++i) {
      impl_->destroy_image(impl_->hist_color[i]);
      impl_->destroy_image(impl_->hist_depth[i]);
      impl_->destroy_image(impl_->hist_conf[i]);
    }
    impl_->destroy_image(impl_->final_output);
    impl_->destroy_buffer(impl_->readback_staging);

    const VkImageUsageFlags hist_usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int i = 0; i < 2; ++i) {
      impl_->create_image(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, hist_usage, impl_->hist_color[i]);
      impl_->create_image(ow, oh, VK_FORMAT_R32_SFLOAT, hist_usage, impl_->hist_depth[i]);
      impl_->create_image(ow, oh, VK_FORMAT_R16_SFLOAT, hist_usage, impl_->hist_conf[i]);
    }
    impl_->create_image(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        impl_->final_output);
    impl_->create_buffer(out_n * 8ull, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         impl_->readback_staging);
    impl_->alloc_out_w = ow;
    impl_->alloc_out_h = oh;
    impl_->has_history = false;
    need_desc_update = true;
  }

  if (need_desc_update) impl_->update_descriptors();

  // Pack staging upload buffer
  void* mapped = nullptr;
  vkMapMemory(impl_->device, impl_->upload_staging.memory, 0,
              impl_->upload_staging.size, 0, &mapped);
  auto* base = static_cast<std::uint8_t*>(mapped);
  const std::size_t off_col = 0;
  const std::size_t off_dep = ren_n * 16ull;
  const std::size_t off_mv = off_dep + ren_n * 4ull;
  const std::size_t off_react = off_mv + ren_n * 8ull;
  const std::size_t off_transp = off_react + ren_n * 4ull;

  auto* p_col = reinterpret_cast<float*>(base + off_col);
  for (std::size_t i = 0; i < ren_n; ++i) {
    p_col[i * 4 + 0] = std::isfinite(inputs.color_hdr[i].r) ? std::max(0.0f, inputs.color_hdr[i].r) : 0.0f;
    p_col[i * 4 + 1] = std::isfinite(inputs.color_hdr[i].g) ? std::max(0.0f, inputs.color_hdr[i].g) : 0.0f;
    p_col[i * 4 + 2] = std::isfinite(inputs.color_hdr[i].b) ? std::max(0.0f, inputs.color_hdr[i].b) : 0.0f;
    p_col[i * 4 + 3] = 1.0f;
  }
  auto* p_dep = reinterpret_cast<float*>(base + off_dep);
  for (std::size_t i = 0; i < ren_n; ++i) {
    p_dep[i] = std::isfinite(inputs.depth[i]) ? inputs.depth[i] : (inputs.reversed_z ? 0.0f : 1.0f);
  }
  auto* p_mv = reinterpret_cast<float*>(base + off_mv);
  for (std::size_t i = 0; i < ren_n; ++i) {
    p_mv[i * 2 + 0] = std::isfinite(inputs.motion_vectors_px[i].x) ? inputs.motion_vectors_px[i].x : 0.0f;
    p_mv[i * 2 + 1] = std::isfinite(inputs.motion_vectors_px[i].y) ? inputs.motion_vectors_px[i].y : 0.0f;
  }
  auto* p_react = reinterpret_cast<float*>(base + off_react);
  auto* p_transp = reinterpret_cast<float*>(base + off_transp);
  for (std::size_t i = 0; i < ren_n; ++i) {
    p_react[i] = inputs.reactive_mask ? std::clamp(inputs.reactive_mask[i], 0.0f, 1.0f) : 0.0f;
    p_transp[i] = inputs.transparency_mask ? std::clamp(inputs.transparency_mask[i], 0.0f, 1.0f) : 0.0f;
  }
  vkUnmapMemory(impl_->device, impl_->upload_staging.memory);

  const int pp = impl_->ping_pong;
  const int in_idx = pp;
  const int out_idx = 1 - pp;
  const bool do_cut = !impl_->has_history || inputs.camera_cut;

  const float safe_exp = (std::isfinite(inputs.pre_exposure) && inputs.pre_exposure > 1e-4f)
                             ? inputs.pre_exposure : 1.0f;
  const float prev_exp = (std::isfinite(impl_->prev_exposure) && impl_->prev_exposure > 1e-4f)
                             ? impl_->prev_exposure : 1.0f;
  const float exp_ratio = std::clamp(safe_exp / prev_exp, 0.01f, 100.0f);

  vkResetFences(impl_->device, 1, &impl_->fence);
  vkResetCommandBuffer(impl_->cmd_buf, 0);
  VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  vkBeginCommandBuffer(impl_->cmd_buf, &cbbi);
  vkCmdResetQueryPool(impl_->cmd_buf, impl_->query_pool, 0, 4);

  auto upload_img = [&](GpuImage& im, std::size_t offset) {
    impl_->transition_image(impl_->cmd_buf, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {rw, rh, 1};
    vkCmdCopyBufferToImage(impl_->cmd_buf, impl_->upload_staging.buffer, im.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    impl_->transition_image(impl_->cmd_buf, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  };
  upload_img(impl_->color_hdr, off_col);
  upload_img(impl_->depth, off_dep);
  upload_img(impl_->motion_px, off_mv);
  upload_img(impl_->reactive_mask, off_react);
  upload_img(impl_->transparency_mask, off_transp);

  if (impl_->hist_color[in_idx].layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    impl_->transition_image(impl_->cmd_buf, impl_->hist_color[in_idx],
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    impl_->transition_image(impl_->cmd_buf, impl_->hist_depth[in_idx],
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    impl_->transition_image(impl_->cmd_buf, impl_->hist_conf[in_idx],
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }

  impl_->transition_image(impl_->cmd_buf, impl_->hist_color[out_idx], VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  impl_->transition_image(impl_->cmd_buf, impl_->hist_depth[out_idx], VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  impl_->transition_image(impl_->cmd_buf, impl_->hist_conf[out_idx], VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

  const std::uint32_t gx = (ow + 7u) / 8u;
  const std::uint32_t gy = (oh + 7u) / 8u;

  ReconstructPushConstants rpc{
      rw, rh, ow, oh,
      inputs.jitter_offset_px.x, inputs.jitter_offset_px.y,
      exp_ratio, impl_->cfg.disocclusion_depth_threshold,
      (do_cut ? 1u : 0u) | (inputs.reversed_z ? 2u : 0u)};

  vkCmdWriteTimestamp(impl_->cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, impl_->query_pool, 0);
  vkCmdBindPipeline(impl_->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->recon_pipeline);
  vkCmdBindDescriptorSets(impl_->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->recon_pipe_layout, 0, 1, &impl_->recon_sets[pp], 0, nullptr);
  vkCmdPushConstants(impl_->cmd_buf, impl_->recon_pipe_layout,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rpc), &rpc);
  vkCmdDispatch(impl_->cmd_buf, gx, gy, 1);
  vkCmdWriteTimestamp(impl_->cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, impl_->query_pool, 1);

  // Transition unsharpened history_color_out -> SHADER_READ_ONLY_OPTIMAL for sharpen pass
  impl_->transition_image(impl_->cmd_buf, impl_->hist_color[out_idx],
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  impl_->transition_image(impl_->cmd_buf, impl_->final_output, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

  SharpenPushConstants spc{ow, oh, impl_->cfg.sharpness};
  vkCmdWriteTimestamp(impl_->cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, impl_->query_pool, 2);
  vkCmdBindPipeline(impl_->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, impl_->sharp_pipeline);
  vkCmdBindDescriptorSets(impl_->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                          impl_->sharp_pipe_layout, 0, 1, &impl_->sharp_sets[pp], 0, nullptr);
  vkCmdPushConstants(impl_->cmd_buf, impl_->sharp_pipe_layout,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(spc), &spc);
  vkCmdDispatch(impl_->cmd_buf, gx, gy, 1);
  vkCmdWriteTimestamp(impl_->cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, impl_->query_pool, 3);

  impl_->transition_image(impl_->cmd_buf, impl_->final_output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  VkBufferImageCopy rb_region{};
  rb_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  rb_region.imageExtent = {ow, oh, 1};
  vkCmdCopyImageToBuffer(impl_->cmd_buf, impl_->final_output.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         impl_->readback_staging.buffer, 1, &rb_region);
  vkEndCommandBuffer(impl_->cmd_buf);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr,
                  1, &impl_->cmd_buf, 0, nullptr};
  if (vkQueueSubmit(impl_->compute_queue, 1, &si, impl_->fence) != VK_SUCCESS) {
    report.status_message = "vkQueueSubmit failed.";
    return report;
  }
  vkWaitForFences(impl_->device, 1, &impl_->fence, VK_TRUE, UINT64_MAX);

  std::uint64_t ts[4]{};
  if (vkGetQueryPoolResults(impl_->device, impl_->query_pool, 0, 4, sizeof(ts), ts,
                            sizeof(std::uint64_t),
                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
    const double ns_to_ms = static_cast<double>(impl_->timestamp_period_ns) * 1e-6;
    report.reconstruct_gpu_ms = static_cast<double>(ts[1] - ts[0]) * ns_to_ms;
    report.sharpen_gpu_ms = static_cast<double>(ts[3] - ts[2]) * ns_to_ms;
    report.gpu_time_ms = report.reconstruct_gpu_ms + report.sharpen_gpu_ms;
  }

  // Read back RGBA16F output into ColorRgb
  void* rb_ptr = nullptr;
  vkMapMemory(impl_->device, impl_->readback_staging.memory, 0,
              impl_->readback_staging.size, 0, &rb_ptr);
  const auto* h16 = static_cast<const std::uint16_t*>(rb_ptr);
  out_gpu_color.resize(out_n);
  for (std::size_t i = 0; i < out_n; ++i) {
    out_gpu_color[i] = {
        f16_to_f32(h16[i * 4 + 0]),
        f16_to_f32(h16[i * 4 + 1]),
        f16_to_f32(h16[i * 4 + 2]),
    };
  }
  vkUnmapMemory(impl_->device, impl_->readback_staging.memory);

  impl_->ping_pong = out_idx;
  impl_->has_history = true;
  impl_->prev_exposure = safe_exp;

  compute_parity_metrics(cpu_reference_output, out_gpu_color, report);
  report.executed = true;
  report.validation_clean = impl_->validation_clean;
  report.parity_passed =
      report.validation_clean &&
      (cpu_reference_output.empty() ||
       (report.cpu_vs_gpu_psnr_db >= impl_->cfg.min_parity_psnr_db &&
        report.cpu_vs_gpu_mae <= impl_->cfg.max_parity_mae));
  report.status_message = report.parity_passed
                              ? "Vulkan compute parity PASSED"
                              : "Vulkan compute parity FAILED";
  return report;
#endif
}

VulkanParityReport SsrVulkanHarness::run_parity_test(
    const VulkanHarnessConfig& cfg,
    const SsrDispatchInputs& inputs,
    const std::vector<ColorRgb>& cpu_reference_output) {
  SsrVulkanHarness harness;
  std::string status;
  if (!harness.initialize(cfg, status)) {
    VulkanParityReport r{};
    r.executed = false;
    r.status_message = status;
    return r;
  }
  std::vector<ColorRgb> gpu_out;
  return harness.execute_frame(inputs, cpu_reference_output, gpu_out);
}

}  // namespace saige::ssr::gpu
