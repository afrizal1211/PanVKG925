#include <stdlib.h>

#include "panvk_kbase_device.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_kbase_sync.h"
#include "kmod/kbase_kmod.h"
#include "vk_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* The G925 fork ships two capability profiles built from the same tree:
 *
 *   PANVK_G925_EXPERIMENTAL == 0  ("safe")
 *     Advertises the extensions/features that are structurally required for a
 *     Vulkan device to be usable by Winlator/Ludashi/DXVK (promoted-core
 *     aliases, memory-requirement/bind helpers, format lists, driver
 *     properties, dma-buf/native WSI). Hardware behaviour is not over-claimed.
 *
 *   PANVK_G925_EXPERIMENTAL == 1  ("experimental")
 *     Additionally enables the hardware-candidate features that DXVK asks for
 *     but which the upstream fork leaves behind the experiment switch
 *     (multiview, variablePointers, shaderDrawParameters,
 *     fragmentStoresAndAtomics, apiVersion1.2, ...). descriptorIndexing,
 *     bufferDeviceAddress and timelineSemaphore stay off: kbase sync has no
 *     timeline support and those paths are not validated. Keep this variant
 *     opt-in.
 *
 * Either profile can still be overridden at runtime with
 * PANVK_KBASE_EXPERIMENT=<comma list>, exactly like upstream.
 */
#ifndef PANVK_G925_EXPERIMENTAL
#define PANVK_G925_EXPERIMENTAL 0
#endif

/* Items that are safe/structural enough to enable in both profiles. */
static bool
kbase_always(const char *name)
{
   static const char *const always[] = {
      "VK_KHR_driver_properties",
      "VK_KHR_get_memory_requirements2",
      "VK_KHR_bind_memory2",
      "VK_KHR_dedicated_allocation",
      "VK_KHR_maintenance1",
      "VK_KHR_maintenance2",
      "VK_KHR_image_format_list",
      "VK_KHR_format_feature_flags2",
      "VK_KHR_spirv_1_4",
      "VK_KHR_shader_non_semantic_info",
      "VK_KHR_relaxed_block_layout",
      "VK_KHR_storage_buffer_storage_class",
      "VK_EXT_private_data",
      "VK_EXT_debug_marker",
      "VK_EXT_tooling_info",
      NULL,
   };
   for (size_t i = 0; always[i]; i++)
      if (!strcmp(always[i], name))
         return true;
   return false;
}

/* Control/rollback switches. These do not "enable a feature"; they select the
 * API version and gate the promoted-core set. Force-enabling them would wrongly
 * downgrade the device, so they must resolve to false in every profile and can
 * only be turned on explicitly via PANVK_KBASE_EXPERIMENT. */
static bool
kbase_is_control(const char *name)
{
   static const char *const controls[] = {
      "apiVersion1.0",
      "apiVersion1.1",
      NULL,
   };
   for (size_t i = 0; controls[i]; i++)
      if (!strcmp(controls[i], name))
         return true;
   return false;
}

/* True when `name` appears as a comma-separated item of `env`. */
static bool
kbase_list_contains(const char *env, const char *name)
{
   if (!env || !*env)
      return false;

   const size_t name_len = strlen(name);
   for (const char *p = env; *p;) {
      const char *end = strchr(p, ',');
      const size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len == name_len && !memcmp(p, name, len))
         return true;
      if (!end)
         break;
      p = end + 1;
   }
   return false;
}

static bool
kbase_experiment(const char *name)
{
   if (kbase_always(name))
      return true;

   /* Never blanket-enable the rollback controls in the experimental profile. */
   if (kbase_is_control(name))
      return kbase_list_contains(getenv("PANVK_KBASE_EXPERIMENT"), name);

#if PANVK_G925_EXPERIMENTAL
   /* Hardware-candidate features enabled by the experimental profile so DXVK
    * gets the feature set it probes for. Runtime PANVK_KBASE_EXPERIMENT still
    * applies on top. */
   return true;
#else
   return kbase_list_contains(getenv("PANVK_KBASE_EXPERIMENT"), name);
#endif
}

bool
panvk_kbase_wsi_enabled(void)
{
   /* Enabled by default in this fork so Winlator/Ludashi/DXVK can present.
     * Explicitly opt out with PANVK_KBASE_WSI_DISABLE=1. */
   if (getenv("PANVK_KBASE_WSI_DISABLE") != NULL)
      return false;
   return true;
}

bool
panvk_kbase_use_software_wsi(void)
{
   /* kbase presentation defaults to the native path now that dma-heap export
    * is on by default. Force the software readback path with
    * PANVK_KBASE_WSI_SW=1. PANVK_KBASE_WSI_NATIVE is accepted for backward
    * compatibility and also selects the native path. */
   if (getenv("PANVK_KBASE_WSI_SW") != NULL)
      return true;
   return false;
}

static bool
valid_device_path(const char *path)
{
   static const char prefix[] = "/dev/mali";
   if (strncmp(path, prefix, sizeof(prefix) - 1) ||
       strlen(path) >= PANVK_KBASE_PATH_SIZE)
      return false;
   const char *suffix = path + sizeof(prefix) - 1;
   if (!*suffix)
      return false;
   for (; *suffix; suffix++)
      if (*suffix < '0' || *suffix > '9')
         return false;
   return true;
}

enum panvk_kbase_selection
panvk_kbase_select_device(const char *path, const char *restriction,
                          char selected[PANVK_KBASE_PATH_SIZE])
{
   selected[0] = '\0';
   if (!path)
      return PANVK_KBASE_SELECTION_DEFAULT;
   if (!restriction || strcmp(restriction, "kbase") || !valid_device_path(path))
      return PANVK_KBASE_SELECTION_INVALID;
   memcpy(selected, path, strlen(path) + 1);
   return PANVK_KBASE_SELECTION_EXPLICIT;
}

VkResult
panvk_kbase_open_device(const char *path, const struct pan_kmod_allocator *allocator,
                        struct pan_kmod_dev **out)
{
   *out = NULL;
   if (!path || !valid_device_path(path))
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   struct pan_kmod_dev *dev = kbase_kmod_ops.dev_create(
      fd, PAN_KMOD_DEV_FLAG_OWNS_FD, allocator);
   if (!dev) {
      int error = errno;
      close(fd);
      errno = error;
      return panvk_catch_indirect_alloc_failure(VK_ERROR_INCOMPATIBLE_DRIVER);
   }
   if (!kbase_kmod_supports_panvk(dev)) {
      pan_kmod_dev_destroy(dev);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   *out = dev;
   return VK_SUCCESS;
}

VkResult
panvk_kbase_enumerate_physical_device(struct panvk_instance *instance)
{
   if (instance->kbase.selection != PANVK_KBASE_SELECTION_EXPLICIT)
      return VK_SUCCESS;
   struct panvk_physical_device *device = vk_zalloc(
      &instance->vk.alloc, sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   VkResult result = panvk_physical_device_init_kbase(device, instance,
                                                     instance->kbase.path);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, device);
      return result == VK_ERROR_INCOMPATIBLE_DRIVER ? VK_SUCCESS : result;
   }
   list_addtail(&device->vk.link, &instance->vk.physical_devices.list);
   return VK_SUCCESS;
}

void
panvk_kbase_physical_device_capabilities(struct vk_device_extension_table *extensions,
                                        struct vk_features *features)
{
   if (extensions) {
      *extensions = (struct vk_device_extension_table){0};
      const bool rollback_1_0 = kbase_experiment("apiVersion1.0");
      /* dma-buf import is implemented by kbase_kmod_bo_import().  Export is
       * still ENOTSUP, so only the importable side is advertised here. */
      extensions->KHR_external_memory = true;
      extensions->KHR_external_memory_fd = true;
      extensions->EXT_external_memory_dma_buf = true;
      /* kbase cannot export dma-buf, so presentation uses the software WSI
       * path. It is on by default here so Windows apps get a swapchain. */
      if (panvk_kbase_wsi_enabled())
         extensions->KHR_swapchain = true;

      /* Verification-only candidates. Never set PANVK_KBASE_EXPERIMENT in a
       * production environment; an item moves out of this block only after
       * its per-item hardware criterion passes. */
      extensions->KHR_driver_properties =
         !rollback_1_0 || kbase_experiment("VK_KHR_driver_properties");
      extensions->KHR_get_memory_requirements2 =
         kbase_experiment("VK_KHR_get_memory_requirements2");
      extensions->KHR_bind_memory2 = kbase_experiment("VK_KHR_bind_memory2");
      extensions->KHR_dedicated_allocation =
         kbase_experiment("VK_KHR_dedicated_allocation");
      extensions->KHR_maintenance1 = kbase_experiment("VK_KHR_maintenance1");
      extensions->KHR_maintenance2 = kbase_experiment("VK_KHR_maintenance2");
      extensions->KHR_maintenance3 = !rollback_1_0 || kbase_experiment("VK_KHR_maintenance3");
      extensions->KHR_maintenance4 = !rollback_1_0 || kbase_experiment("VK_KHR_maintenance4");
      extensions->KHR_maintenance5 = !rollback_1_0 || kbase_experiment("VK_KHR_maintenance5");
      extensions->KHR_copy_commands2 = !rollback_1_0 || kbase_experiment("VK_KHR_copy_commands2");
      extensions->KHR_create_renderpass2 =
         !rollback_1_0 || kbase_experiment("VK_KHR_create_renderpass2");
      extensions->KHR_image_format_list =
         kbase_experiment("VK_KHR_image_format_list");
      extensions->KHR_format_feature_flags2 =
         kbase_experiment("VK_KHR_format_feature_flags2");
      extensions->KHR_map_memory2 =
         !rollback_1_0 || kbase_experiment("VK_KHR_map_memory2");
      extensions->KHR_relaxed_block_layout =
         kbase_experiment("VK_KHR_relaxed_block_layout");
      extensions->KHR_storage_buffer_storage_class =
         kbase_experiment("VK_KHR_storage_buffer_storage_class");
      extensions->KHR_spirv_1_4 = kbase_experiment("VK_KHR_spirv_1_4");
      extensions->KHR_shader_non_semantic_info =
         kbase_experiment("VK_KHR_shader_non_semantic_info");
      extensions->EXT_tooling_info = kbase_experiment("VK_EXT_tooling_info");
      extensions->EXT_private_data = !rollback_1_0 || kbase_experiment("VK_EXT_private_data");
      extensions->EXT_debug_marker = kbase_experiment("VK_EXT_debug_marker");
   }
   if (features) {
      *features = (struct vk_features){
         /* Each of these was checked against hardware behaviour, not read off
          * the vendor source. See the probe archives under
          * mesa-port-backups/ for the exact evidence behind each one. */
         /* s10: an isotropic sampler averages a one-texel stripe texture to
          * flat grey while the anisotropic sampler keeps the full range. */
         .samplerAnisotropy = true,
         /* s11: a quad entirely beyond the far plane draws nothing with
          * depthClamp off and covers the viewport with it on. */
         .depthClamp = true,
         /* s12: the same bias moves the sampled depth to 0.488 unclamped and
          * stops at 0.300 when clamped. */
         .depthBiasClamp = true,
         /* s13: additive and replace states on two attachments give
          * 255,64,64 and 255,0,0 respectively. */
         .independentBlend = true,
      };

      /* Minimum Vulkan 1.1 declaration: only the three features whose
       * hardware criteria passed.  multiview and every other 1.1 feature
       * stay false by default.  The experiment switch remains available for
       * retesting candidates and for an explicit apiVersion1.0 rollback. */
      const bool rollback_1_0 = kbase_experiment("apiVersion1.0");
      const bool rollback_1_1 = kbase_experiment("apiVersion1.1");
      const bool api12 = !rollback_1_0 && !rollback_1_1 &&
                         kbase_experiment("apiVersion1.2");
      features->storageBuffer16BitAccess = !rollback_1_0;
      features->uniformAndStorageBuffer16BitAccess = !rollback_1_0;
      features->storagePushConstant16 = !rollback_1_0;
      features->storageInputOutput16 = false;
      features->multiview = kbase_experiment("multiview");
      features->variablePointers = kbase_experiment("variablePointers");
      features->variablePointersStorageBuffer =
         kbase_experiment("variablePointersStorageBuffer");
      features->samplerYcbcrConversion =
         kbase_experiment("samplerYcbcrConversion");
      features->shaderDrawParameters =
         kbase_experiment("shaderDrawParameters");
      features->fragmentStoresAndAtomics =
         kbase_experiment("fragmentStoresAndAtomics");
      features->vertexPipelineStoresAndAtomics =
         kbase_experiment("vertexPipelineStoresAndAtomics");

      /* Minimum Vulkan 1.2 declaration: seven features with semantic
       * evidence.  descriptorIndexing, bufferDeviceAddress, vulkanMemoryModel,
       * shaderSubgroupExtendedTypes and timelineSemaphore stay false. */
      features->scalarBlockLayout = api12;
      features->uniformBufferStandardLayout = api12;
      features->storageBuffer8BitAccess = api12;
      features->hostQueryReset = api12;
      features->imagelessFramebuffer = api12;
      features->separateDepthStencilLayouts = api12;
      features->drawIndirectCount = api12;

      features->descriptorIndexing = false;
      features->runtimeDescriptorArray = false;
      features->descriptorBindingUniformBufferUpdateAfterBind = false;
      features->descriptorBindingSampledImageUpdateAfterBind = false;
      features->descriptorBindingStorageImageUpdateAfterBind = false;
      features->descriptorBindingStorageBufferUpdateAfterBind = false;
      features->descriptorBindingUniformTexelBufferUpdateAfterBind = false;
      features->descriptorBindingStorageTexelBufferUpdateAfterBind = false;
      features->descriptorBindingUpdateUnusedWhilePending = false;
      features->descriptorBindingPartiallyBound = false;
      features->descriptorBindingVariableDescriptorCount = false;
      features->bufferDeviceAddress = false;
      features->vulkanMemoryModel = false;
      features->vulkanMemoryModelDeviceScope = false;
      features->shaderSubgroupExtendedTypes = false;

      /* Hard constraint: kbase sync type has no timeline support. */
      features->timelineSemaphore = false;
   }
}

void
panvk_kbase_physical_device_setup(struct panvk_physical_device *device)
{
   assert(panvk_physical_device_uses_kbase(device));
   memset(device->sync_types, 0, sizeof(device->sync_types));
   device->sync_types[0] = &panvk_kbase_sync_type;
   device->vk.supported_sync_types = device->sync_types;
}

void
panvk_kbase_physical_device_properties(struct vk_properties *properties)
{
   /* Minimum 1.1 is the default.  apiVersion1.0 in the experiment switch is
    * the explicit rollback path. */
   properties->apiVersion = kbase_experiment("apiVersion1.0") ?
      VK_API_VERSION_1_0 :
      (kbase_experiment("apiVersion1.1") ? VK_API_VERSION_1_1 :
       (kbase_experiment("apiVersion1.2") ? VK_API_VERSION_1_2 :
        VK_API_VERSION_1_1));
   properties->conformanceVersion = (VkConformanceVersion){0};
   properties->timestampComputeAndGraphics = false;
   properties->timestampPeriod = 1.0f;
}
