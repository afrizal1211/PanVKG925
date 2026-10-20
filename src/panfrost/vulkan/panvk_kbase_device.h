#ifndef PANVK_KBASE_DEVICE_H
#define PANVK_KBASE_DEVICE_H

#include <stdbool.h>
#include <vulkan/vulkan_core.h>

struct panvk_instance;
struct panvk_physical_device;
struct vk_properties;
struct pan_kmod_dev;
struct pan_kmod_allocator;
struct vk_device_extension_table;
struct vk_features;

#define PANVK_KBASE_PATH_SIZE 64

enum panvk_kbase_selection {
   PANVK_KBASE_SELECTION_DEFAULT,
   PANVK_KBASE_SELECTION_EXPLICIT,
   PANVK_KBASE_SELECTION_INVALID,
};

/* True when the kbase WSI path should be enabled. The G925 build turns this on
 * by default so Windows apps get a swapchain; set PANVK_KBASE_WSI_DISABLE=1 at
 * runtime to force it off. */
bool panvk_kbase_wsi_enabled(void);

/* True when presentation must use the software (readback) WSI path instead of
 * the default native dma-buf path. Opt in with PANVK_KBASE_WSI_SW=1. */
bool panvk_kbase_use_software_wsi(void);

enum panvk_kbase_selection panvk_kbase_select_device(
   const char *path, const char *restriction, char selected[PANVK_KBASE_PATH_SIZE]);
VkResult panvk_kbase_open_device(const char *path,
                                 const struct pan_kmod_allocator *allocator,
                                 struct pan_kmod_dev **out);
VkResult panvk_kbase_enumerate_physical_device(struct panvk_instance *instance);
void panvk_kbase_physical_device_capabilities(struct vk_device_extension_table *extensions,
                                             struct vk_features *features);
void panvk_kbase_physical_device_setup(struct panvk_physical_device *device);
void panvk_kbase_physical_device_properties(struct vk_properties *properties);

#endif
