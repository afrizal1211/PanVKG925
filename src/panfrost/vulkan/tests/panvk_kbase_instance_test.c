#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_entrypoints.h"
#include "vk_alloc.h"
#include "vk_common_entrypoints.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../panvk_instance.c"

const struct vk_instance_entrypoint_table panvk_instance_entrypoints = {
   .EnumerateInstanceVersion = panvk_EnumerateInstanceVersion,
};
const struct vk_instance_entrypoint_table wsi_instance_entrypoints = {0};

static unsigned kbase_initializations;
static unsigned drm_initializations;

VkResult
panvk_physical_device_init(struct panvk_physical_device *device,
                           struct panvk_instance *instance, drmDevicePtr drm_device)
{
   drm_initializations++;
   abort();
}

VkResult
panvk_physical_device_init_kbase(struct panvk_physical_device *device,
                                 struct panvk_instance *instance, const char *path)
{
   kbase_initializations++;
   assert(!strcmp(path, instance->kbase.path));
   return VK_ERROR_INCOMPATIBLE_DRIVER;
}

void
panvk_physical_device_finish(struct panvk_physical_device *device)
{
   abort();
}

void
panvk_parse_dri_options(struct panvk_drirc *drirc,
                        const driConfigFileParseParams *params)
{
   drirc->debug.force_vk_devicename = "";
   drirc->misc.compute_core_mask = UINT64_MAX;
   drirc->misc.fragment_core_mask = UINT64_MAX;
   drirc->misc.heap_memory_percent = 50;
}

static VkResult
create_instance(VkInstance *instance, const char *extension)
{
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = extension ? 1 : 0,
                               .ppEnabledExtensionNames = &extension};
   return panvk_CreateInstance(&info, NULL, instance);
}

int
main(void)
{
   unsetenv("PANVK_KBASE_DEVICE");
   unsetenv("PAN_KMOD_RESTRICT_TO_BACKENDS");
   VkInstance drm_instance;
   assert(create_instance(&drm_instance, NULL) == VK_SUCCESS);
   struct panvk_instance *drm = panvk_instance_from_handle(drm_instance);
   assert(drm->kbase.selection == PANVK_KBASE_SELECTION_DEFAULT);
   assert(drm->vk.physical_devices.try_create_for_drm == panvk_physical_device_try_create);
   assert(!drm->vk.physical_devices.enumerate);
   assert(drm->vk.supported_extensions == &panvk_instance_extensions);
   setenv("PANVK_KBASE_DEVICE", "/dev/mali0", 1);
   VkInstance invalid;
   assert(create_instance(&invalid, NULL) == VK_ERROR_INITIALIZATION_FAILED);
   assert(!kbase_initializations && !drm_initializations);
   setenv("PAN_KMOD_RESTRICT_TO_BACKENDS", "kbase", 1);
   /* Surface extensions are declared so the software presentation path can
    * create a swapchain; without the opt-in nothing else claims them yet. */
   assert(create_instance(&invalid, VK_KHR_SURFACE_EXTENSION_NAME) == VK_SUCCESS);
   VkInstance selected;
   assert(create_instance(&selected, NULL) == VK_SUCCESS);
   struct panvk_instance *instance = panvk_instance_from_handle(selected);
   assert(!instance->vk.physical_devices.try_create_for_drm);
   assert(instance->vk.physical_devices.enumerate == panvk_enumerate_kbase);
   assert(instance->vk.supported_extensions == &panvk_kbase_instance_extensions);
   assert(instance->vk.supported_extensions->KHR_get_physical_device_properties2);
   assert(instance->vk.supported_extensions->KHR_surface);
   setenv("PANVK_KBASE_DEVICE", "/dev/mali1", 1);
   setenv("PAN_KMOD_RESTRICT_TO_BACKENDS", "all", 1);
   assert(!strcmp(instance->kbase.path, "/dev/mali0"));
   uint32_t count = 123;
   assert(vk_common_EnumeratePhysicalDevices(selected, &count, NULL) == VK_SUCCESS);
   assert(!count && kbase_initializations == 1 && !drm_initializations);
   assert(vk_common_EnumeratePhysicalDevices(selected, &count, NULL) == VK_SUCCESS);
   assert(!count && kbase_initializations == 1);
   panvk_DestroyInstance(selected, NULL);
   panvk_DestroyInstance(drm_instance, NULL);
   return 0;
}
