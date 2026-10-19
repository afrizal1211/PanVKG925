#include "panvk_physical_device.h"
#include "panvk_kbase_device.h"
#include "panvk_kbase_sync.h"
#include "panvk_entrypoints.h"
#include "vk_alloc.h"
#include "vk_drm_syncobj.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

int __wrap_open(const char *path, int flags, ...);
int __wrap_open64(const char *path, int flags, ...);
int __wrap_ioctl(int fd, unsigned long request, ...);
void *__wrap_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset);
void *__wrap_mmap64(void *address, size_t length, int prot, int flags, int fd, off64_t offset);

int __wrap_open(const char *path, int flags, ...) { abort(); }
int __wrap_open64(const char *path, int flags, ...) { abort(); }
int __wrap_ioctl(int fd, unsigned long request, ...) { abort(); }
void *__wrap_mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset) { abort(); }
void *__wrap_mmap64(void *address, size_t length, int prot, int flags, int fd, off64_t offset) { abort(); }

#include "../panvk_physical_device.c"

uint64_t panvk_debug;
const struct vk_physical_device_entrypoint_table panvk_physical_device_entrypoints = {0};
const struct vk_physical_device_entrypoint_table wsi_physical_device_entrypoints = {0};

static struct pan_kmod_dev fake_dev;
static unsigned destroyed;
static bool incompatible;
static unsigned cache_calls;
static char cache_renderer[64];

struct disk_cache *__wrap_disk_cache_create(const char *renderer, const char *timestamp, uint64_t driver_flags);

struct disk_cache *
__wrap_disk_cache_create(const char *renderer, const char *timestamp, uint64_t driver_flags)
{
   assert(!strcmp(renderer, "panvk_kbase_m4a_v1_0x00000000d8300000"));
   assert(strlen(timestamp) && driver_flags == 0);
   strcpy(cache_renderer, renderer);
   cache_calls++;
   return NULL;
}

static struct pan_kmod_va_range
query_va(const struct pan_kmod_dev *dev)
{
   return (struct pan_kmod_va_range){.start = 0x2000000, .size = (1ull << 47) - 0x2000000};
}

const struct pan_kmod_ops __wrap_kbase_kmod_ops = {.dev_query_user_va_range = query_va};

VkResult __wrap_panvk_kbase_open_device(const char *, const struct pan_kmod_allocator *, struct pan_kmod_dev **);
void __wrap_pan_kmod_dev_destroy(struct pan_kmod_dev *dev);
struct vk_sync_type __wrap_vk_drm_syncobj_get_type_from_provider(struct util_sync_provider *provider);
uint32_t __wrap_pan_get_compiler_flags(unsigned arch);
struct pan_kmod_dev *__wrap_pan_kmod_dev_create(int fd, uint32_t flags, const struct pan_kmod_allocator *allocator);

uint32_t
__wrap_pan_get_compiler_flags(unsigned arch)
{
   assert(arch == 13);
   return 0;
}

struct pan_kmod_dev *
__wrap_pan_kmod_dev_create(int fd, uint32_t flags, const struct pan_kmod_allocator *allocator)
{
   abort();
}

bool
wsi_instance_supports_google_display_timing(const struct vk_instance *instance,
                                           const struct driOptionCache *options)
{
   abort();
}

VkResult
__wrap_panvk_kbase_open_device(const char *path, const struct pan_kmod_allocator *allocator,
                        struct pan_kmod_dev **out)
{
   assert(!strcmp(path, "/dev/mali0"));
   *out = incompatible ? NULL : &fake_dev;
   return incompatible ? VK_ERROR_INCOMPATIBLE_DRIVER : VK_SUCCESS;
}

void
__wrap_pan_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   assert(dev == &fake_dev);
   destroyed++;
}

VkResult
panvk_wsi_init(struct panvk_physical_device *device)
{
   abort();
}

void
panvk_wsi_finish(struct panvk_physical_device *device)
{
   abort();
}

struct vk_sync_type
__wrap_vk_drm_syncobj_get_type_from_provider(struct util_sync_provider *provider)
{
   abort();
}

#define UNUSED_ARCH(arch) \
void panvk_v##arch##_get_physical_device_extensions(const struct panvk_physical_device *d, const struct panvk_instance *i, struct vk_device_extension_table *e) { abort(); } \
void panvk_v##arch##_get_physical_device_features(const struct panvk_instance *i, const struct panvk_physical_device *d, struct vk_features *f) { abort(); } \
void panvk_v##arch##_get_physical_device_properties(const struct panvk_instance *i, const struct panvk_physical_device *d, struct vk_properties *p) { abort(); }

UNUSED_ARCH(6)
UNUSED_ARCH(7)
UNUSED_ARCH(10)
UNUSED_ARCH(11)
UNUSED_ARCH(12)
UNUSED_ARCH(14)

static void
fixture_init(struct panvk_instance *instance)
{
   memset(instance, 0, sizeof(*instance));
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   const struct vk_instance_extension_table extensions = {0};
   const struct vk_instance_dispatch_table dispatch = {0};
   assert(vk_instance_init(&instance->vk, &extensions, &dispatch, &info, vk_default_allocator()) == VK_SUCCESS);
   instance->kbase.selection = PANVK_KBASE_SELECTION_EXPLICIT;
   strcpy(instance->kbase.path, "/dev/mali0");
   instance->drirc.debug.force_vk_devicename = "";
   instance->drirc.misc.compute_core_mask = UINT64_MAX;
   instance->drirc.misc.fragment_core_mask = UINT64_MAX;
   instance->drirc.misc.heap_memory_percent = 50;
   fake_dev = (struct pan_kmod_dev){
      .ops = &kbase_kmod_ops,
      .props = {
         .gpu_id = 0xd8300000, .gpu_variant = 4,
         .gpu_features = 1ull << 2, .shader_present = 0xfff,
         .mmu_features = 48, .max_threads_per_wg = 1024,
         .max_threads_per_core = 2048, .max_tls_instance_per_core = 2048,
         .tiler_features = (8 << 8) | 9,
         .supported_bo_flags = PAN_KMOD_BO_FLAG_WB_MMAP,
      },
   };
}

int
main(void)
{
   setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
   struct panvk_instance instance;
   fixture_init(&instance);
   struct panvk_physical_device device = {0};
   assert(panvk_physical_device_init_kbase(&device, &instance, "/dev/mali0") == VK_SUCCESS);
   assert(device.vk.instance == &instance.vk && device.model && !device.vk.wsi_device);
   assert(device.vk.properties.apiVersion == VK_API_VERSION_1_0);
   assert(device.vk.supported_sync_types[0] == &panvk_kbase_sync_type);
   assert(!device.vk.supported_sync_types[1]);
   assert(!device.vk.supported_features.timelineSemaphore && !device.vk.supported_features.sparseBinding);
   assert(!device.vk.supported_extensions.KHR_external_memory_fd && !device.vk.supported_extensions.KHR_swapchain);
   assert(device.csf.tiler.chunk_size == 1024 * 1024);
   uint32_t count = 1;
   VkQueueFamilyProperties2 family = {.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2};
   panvk_GetPhysicalDeviceQueueFamilyProperties2(panvk_physical_device_to_handle(&device), &count, &family);
   assert(count == 1 && family.queueFamilyProperties.queueCount == 1);
   assert(family.queueFamilyProperties.timestampValidBits == 0);
   assert(!(family.queueFamilyProperties.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT));
   VkPhysicalDeviceExternalBufferInfo external_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   VkExternalBufferProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
   panvk_GetPhysicalDeviceExternalBufferProperties(panvk_physical_device_to_handle(&device), &external_info, &external);
   assert(!external.externalMemoryProperties.externalMemoryFeatures);
   assert(!external.externalMemoryProperties.compatibleHandleTypes);
   assert(!external.externalMemoryProperties.exportFromImportedHandleTypes);
   uint8_t cache_uuid[VK_UUID_SIZE];
   assert(cache_calls == 1 && strstr(cache_renderer, "kbase_m4a_v1"));
   memcpy(cache_uuid, device.cache_uuid, sizeof(cache_uuid));
   panvk_physical_device_finish(&device);
   assert(destroyed == 1);
   memset(&device, 0, sizeof(device));
   instance.drirc.misc.compute_core_mask = 0;
   assert(panvk_physical_device_init_kbase(&device, &instance, "/dev/mali0") == VK_ERROR_INITIALIZATION_FAILED);
   assert(!device.kmod.dev && !device.vk.instance && destroyed == 2);
   instance.drirc.misc.compute_core_mask = UINT64_MAX;
   memset(&device, 0, sizeof(device));
   assert(panvk_physical_device_init_kbase(&device, &instance, "/dev/mali0") == VK_SUCCESS);
   assert(!memcmp(cache_uuid, device.cache_uuid, sizeof(cache_uuid)));
   panvk_physical_device_finish(&device);
   assert(destroyed == 3);
   incompatible = true;
   memset(&device, 0, sizeof(device));
   assert(panvk_physical_device_init_kbase(&device, &instance, "/dev/mali0") == VK_ERROR_INCOMPATIBLE_DRIVER);
   assert(!device.vk.instance && destroyed == 3);
   vk_instance_finish(&instance.vk);
   return 0;
}
