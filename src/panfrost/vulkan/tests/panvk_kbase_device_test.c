#include "panvk_kbase_device.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_kbase_sync.h"
#include "vk_alloc.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static struct pan_kmod_dev fake_dev;
static unsigned opens, closes, creates, destroys, initializes;
static bool fail_open, fail_create, host_oom, supported = true;
static VkResult init_result;
static char last_path[PANVK_KBASE_PATH_SIZE];
static const struct pan_kmod_allocator allocator = {0};

int __wrap_open(const char *path, int flags, ...);
int __wrap_open64(const char *path, int flags, ...);
int __wrap_close(int fd);

int
__wrap_open(const char *path, int flags, ...)
{
   assert(flags == (O_RDWR | O_CLOEXEC));
   assert(strlen(path) < sizeof(last_path));
   strcpy(last_path, path);
   opens++;
   if (fail_open) {
      errno = ENOENT;
      return -1;
   }
   return 123;
}

int
__wrap_open64(const char *path, int flags, ...)
{
   return __wrap_open(path, flags);
}

int
__wrap_close(int fd)
{
   assert(fd == 123);
   closes++;
   return 0;
}

static struct pan_kmod_dev *
create_device(int fd, uint32_t flags, const struct pan_kmod_allocator *alloc)
{
   assert(fd == 123 && flags == PAN_KMOD_DEV_FLAG_OWNS_FD && alloc == &allocator);
   creates++;
   if (fail_create) {
      errno = host_oom ? -ENOMEM : EINVAL;
      return NULL;
   }
   fake_dev = (struct pan_kmod_dev){.fd = fd, .flags = flags, .ops = &kbase_kmod_ops};
   return &fake_dev;
}

const struct pan_kmod_ops kbase_kmod_ops = {.dev_create = create_device};

bool
kbase_kmod_supports_panvk(const struct pan_kmod_dev *dev)
{
   assert(dev == &fake_dev);
   return supported;
}

void
pan_kmod_dev_destroy(struct pan_kmod_dev *dev)
{
   assert(dev == &fake_dev);
   destroys++;
   __wrap_close(dev->fd);
}

VkResult
panvk_physical_device_init_kbase(struct panvk_physical_device *device,
                                 struct panvk_instance *instance, const char *path)
{
   initializes++;
   assert(instance->kbase.selection == PANVK_KBASE_SELECTION_EXPLICIT);
   assert(!strcmp(path, instance->kbase.path));
   if (init_result != VK_SUCCESS)
      return init_result;
   device->vk.instance = &instance->vk;
   device->kmod.dev = &fake_dev;
   return VK_SUCCESS;
}

static void
reset(void)
{
   opens = closes = creates = destroys = initializes = 0;
   fail_open = fail_create = host_oom = false;
   supported = true;
   init_result = VK_SUCCESS;
   last_path[0] = 0;
}

static void
test_selection(void)
{
   char selected[PANVK_KBASE_PATH_SIZE];
   assert(panvk_kbase_select_device(NULL, NULL, selected) == PANVK_KBASE_SELECTION_DEFAULT);
   assert(!selected[0]);
   assert(panvk_kbase_select_device(NULL, "kbase", selected) == PANVK_KBASE_SELECTION_DEFAULT);
   const char *bad_paths[] = {"", "mali0", "/dev/mali", "/dev/mali-1", "/dev/mali0/../mem", "/dev/mali0x", "/home/zl/mali0"};
   for (unsigned i = 0; i < ARRAY_SIZE(bad_paths); i++) {
      assert(panvk_kbase_select_device(bad_paths[i], "kbase", selected) == PANVK_KBASE_SELECTION_INVALID);
      assert(!selected[0]);
   }
   const char *bad_restrictions[] = {NULL, "", "all", "kbase,panthor", " kbase"};
   for (unsigned i = 0; i < ARRAY_SIZE(bad_restrictions); i++)
      assert(panvk_kbase_select_device("/dev/mali0", bad_restrictions[i], selected) == PANVK_KBASE_SELECTION_INVALID);
   assert(panvk_kbase_select_device("/dev/mali0", "kbase", selected) == PANVK_KBASE_SELECTION_EXPLICIT);
   assert(!strcmp(selected, "/dev/mali0") && !opens);
}

static void
test_open(void)
{
   reset();
   struct pan_kmod_dev *out = (void *)1;
   assert(panvk_kbase_open_device("/home/zl/invalid", &allocator, &out) == VK_ERROR_INCOMPATIBLE_DRIVER);
   assert(!out && !opens);
   fail_open = true;
   assert(panvk_kbase_open_device("/dev/mali0", &allocator, &out) == VK_ERROR_INCOMPATIBLE_DRIVER);
   assert(!out && opens == 1 && creates == 0 && !closes);
   fail_open = false;
   fail_create = true;
   assert(panvk_kbase_open_device("/dev/mali0", &allocator, &out) == VK_ERROR_INCOMPATIBLE_DRIVER);
   assert(!out && creates == 1 && closes == 1);
   host_oom = true;
   assert(panvk_kbase_open_device("/dev/mali0", &allocator, &out) == VK_ERROR_OUT_OF_HOST_MEMORY);
   assert(errno == 0 && !out && closes == 2);
   fail_create = false;
   supported = false;
   assert(panvk_kbase_open_device("/dev/mali0", &allocator, &out) == VK_ERROR_INCOMPATIBLE_DRIVER);
   assert(!out && destroys == 1 && closes == 3);
   supported = true;
   assert(panvk_kbase_open_device("/dev/mali0", &allocator, &out) == VK_SUCCESS);
   assert(out == &fake_dev && closes == 3 && !strcmp(last_path, "/dev/mali0"));
   pan_kmod_dev_destroy(out);
   assert(closes == 4 && destroys == 2);
}

static void
test_enumerate(void)
{
   reset();
   struct panvk_instance instance = {0};
   instance.vk.alloc = *vk_default_allocator();
   list_inithead(&instance.vk.physical_devices.list);
   assert(panvk_kbase_enumerate_physical_device(&instance) == VK_SUCCESS);
   assert(!initializes && list_is_empty(&instance.vk.physical_devices.list));
   char path[] = "/dev/mali0";
   instance.kbase.selection = panvk_kbase_select_device(path, "kbase", instance.kbase.path);
   strcpy(path, "/dev/mali1");
   assert(!strcmp(instance.kbase.path, "/dev/mali0"));
   struct panvk_instance snapshot = {0};
   snapshot.kbase.selection = panvk_kbase_select_device("/dev/mali0", "kbase", snapshot.kbase.path);
   instance.kbase.selection = panvk_kbase_select_device("/dev/mali1", "kbase", instance.kbase.path);
   assert(!strcmp(snapshot.kbase.path, "/dev/mali0"));
   assert(!strcmp(instance.kbase.path, "/dev/mali1"));
   init_result = VK_ERROR_INCOMPATIBLE_DRIVER;
   assert(panvk_kbase_enumerate_physical_device(&instance) == VK_SUCCESS);
   assert(initializes == 1 && list_is_empty(&instance.vk.physical_devices.list));
   init_result = VK_ERROR_OUT_OF_HOST_MEMORY;
   assert(panvk_kbase_enumerate_physical_device(&instance) == VK_ERROR_OUT_OF_HOST_MEMORY);
   assert(initializes == 2 && list_is_empty(&instance.vk.physical_devices.list));
   init_result = VK_SUCCESS;
   assert(panvk_kbase_enumerate_physical_device(&instance) == VK_SUCCESS);
   assert(initializes == 3 && !list_is_empty(&instance.vk.physical_devices.list));
   struct vk_physical_device *vk = list_first_entry(&instance.vk.physical_devices.list, struct vk_physical_device, link);
   list_del(&vk->link);
   vk_free(&instance.vk.alloc, to_panvk_physical_device(vk));
}

static void
test_policy(void)
{
   struct panvk_physical_device device = {0};
   device.kmod.dev = &fake_dev;
   fake_dev.ops = &kbase_kmod_ops;
   panvk_kbase_physical_device_setup(&device);
   assert(device.vk.supported_sync_types == device.sync_types);
   assert(device.sync_types[0] == &panvk_kbase_sync_type && !device.sync_types[1]);
   assert(!(device.sync_types[0]->features & (VK_SYNC_FEATURE_GPU_WAIT | VK_SYNC_FEATURE_TIMELINE)));
   struct vk_device_extension_table ext;
   struct vk_features features;
   memset(&ext, 0xff, sizeof(ext));
   memset(&features, 0xff, sizeof(features));
   panvk_kbase_physical_device_capabilities(&ext, &features);
   const struct vk_device_extension_table no_ext = {0};
   assert(!memcmp(&ext, &no_ext, sizeof(ext)));
   /* Only features with a hardware check behind them are declared; everything
    * else must stay off. */
   const struct vk_features kbase_features = {
      .samplerAnisotropy = true,
      .depthClamp = true,
      .depthBiasClamp = true,
      .independentBlend = true,
   };
   assert(!memcmp(&features, &kbase_features, sizeof(features)));
   struct vk_properties props = {.apiVersion = VK_API_VERSION_1_4,
      .timestampComputeAndGraphics = true, .timestampPeriod = 10,
      .conformanceVersion = {1, 4, 1, 2}};
   panvk_kbase_physical_device_properties(&props);
   assert(props.apiVersion == VK_API_VERSION_1_0 && !props.timestampComputeAndGraphics);
   assert(props.timestampPeriod == 1.0f && !props.conformanceVersion.major);
}

int
main(void)
{
   test_selection();
   test_open();
   test_enumerate();
   test_policy();
   return 0;
}
