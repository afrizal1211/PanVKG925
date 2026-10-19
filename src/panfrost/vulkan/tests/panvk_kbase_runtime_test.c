#include "panvk_device.h"
#include "panvk_kbase_device.h"
#include "panvk_kbase_sync.h"
#include "panvk_physical_device.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kmod/kbase_kmod.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_fence.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_queue.h"
#include "vk_semaphore.h"
#include "vk_sync.h"

const struct pan_kmod_ops kbase_kmod_ops = {0};

struct runtime_fixture {
   struct panvk_instance instance;
   struct panvk_physical_device physical_device;
   struct panvk_device device;
   struct vk_queue queue;
   struct pan_kmod_dev kdev;
   VkAllocationCallbacks allocation_callbacks;
   bool fail_allocation;
};

static void *
test_alloc(void *user_data, size_t size, size_t alignment,
           VkSystemAllocationScope scope)
{
   struct runtime_fixture *fixture = user_data;
   if (fixture->fail_allocation)
      return NULL;

   if (alignment < sizeof(void *))
      alignment = sizeof(void *);
   void *ptr = NULL;
   if (posix_memalign(&ptr, alignment, size) != 0)
      return NULL;
   return ptr;
}

static void *
test_realloc(void *user_data, void *original, size_t size,
              size_t alignment, VkSystemAllocationScope scope)
{
   if (original == NULL)
      return test_alloc(user_data, size, alignment, scope);
   if (size == 0) {
      free(original);
      return NULL;
   }

   struct runtime_fixture *fixture = user_data;
   if (fixture->fail_allocation)
      return NULL;
   return realloc(original, size);
}

static void
test_free(void *user_data, void *memory)
{
   free(memory);
}

static VkResult
mock_driver_submit(struct vk_queue *queue, struct vk_queue_submit *submit)
{
   struct vk_device *device = queue->base.device;
   for (uint32_t i = 0; i < submit->signal_count; i++) {
      VkResult result = vk_sync_signal(device, submit->signals[i].sync,
                                       submit->signals[i].signal_value);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

static void
fixture_init(struct runtime_fixture *fixture)
{
   memset(fixture, 0, sizeof(*fixture));

   fixture->allocation_callbacks = (VkAllocationCallbacks){
      .pUserData = fixture,
      .pfnAllocation = test_alloc,
      .pfnReallocation = test_realloc,
      .pfnFree = test_free,
   };

   VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_0,
   };
   VkInstanceCreateInfo instance_create_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
   };
   struct vk_instance_dispatch_table instance_dispatch = {0};
   assert(vk_instance_init(&fixture->instance.vk, NULL, &instance_dispatch,
                           &instance_create_info,
                           &fixture->allocation_callbacks) == VK_SUCCESS);
   (void)vk_instance_to_handle(&fixture->instance.vk);

   fixture->kdev.ops = &kbase_kmod_ops;
   fixture->physical_device.kmod.dev = &fixture->kdev;
   struct vk_device_extension_table device_extensions;
   struct vk_features features;
   panvk_kbase_physical_device_capabilities(&device_extensions, &features);
   struct vk_properties properties = {0};
   panvk_kbase_physical_device_properties(&properties);
   struct vk_physical_device_dispatch_table physical_dispatch = {0};
   assert(vk_physical_device_init(&fixture->physical_device.vk,
                                 &fixture->instance.vk, &device_extensions,
                                 &features, &properties,
                                 &physical_dispatch) == VK_SUCCESS);
   panvk_kbase_physical_device_setup(&fixture->physical_device);

   const float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo device_create_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   struct vk_device_dispatch_table device_dispatch = {0};
   vk_device_dispatch_table_from_entrypoints(&device_dispatch,
                                            &vk_common_device_entrypoints,
                                            true);
   device_dispatch.CreateSemaphore = panvk_CreateSemaphore;
   assert(vk_device_init(&fixture->device.vk, &fixture->physical_device.vk,
                         &device_dispatch, &device_create_info,
                         &fixture->allocation_callbacks) == VK_SUCCESS);
   fixture->device.kmod.dev = &fixture->kdev;

   assert(fixture->device.vk.timeline_mode == VK_DEVICE_TIMELINE_MODE_NONE);
   assert(fixture->device.vk.submit_mode == VK_QUEUE_SUBMIT_MODE_IMMEDIATE);
   assert(vk_queue_init(&fixture->queue, &fixture->device.vk, &queue_info, 0) ==
          VK_SUCCESS);
   assert(fixture->queue.submit.mode == VK_QUEUE_SUBMIT_MODE_IMMEDIATE);
   assert(!fixture->queue.submit.thread_run);
   fixture->queue.driver_submit = mock_driver_submit;
}

static void
fixture_finish(struct runtime_fixture *fixture)
{
   vk_queue_finish(&fixture->queue);
   vk_device_finish(&fixture->device.vk);
   vk_physical_device_finish(&fixture->physical_device.vk);
   vk_instance_finish(&fixture->instance.vk);
}

static void
test_binary_semaphore_lifecycle(struct runtime_fixture *fixture)
{
   VkSemaphoreCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
   };
   VkSemaphore semaphore = VK_NULL_HANDLE;
   VkDevice device = vk_device_to_handle(&fixture->device.vk);
   assert(fixture->device.vk.dispatch_table.CreateSemaphore(
             device, &create_info, NULL, &semaphore) == VK_SUCCESS);
   struct vk_semaphore *semaphore_obj = vk_semaphore_from_handle(semaphore);
   assert(semaphore_obj->type == VK_SEMAPHORE_TYPE_BINARY);
   assert(semaphore_obj->permanent.type == &panvk_kbase_sync_type);
   assert(semaphore_obj->permanent.flags == 0);
   assert(vk_sync_wait(&fixture->device.vk, &semaphore_obj->permanent, 0,
                       VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   fixture->device.vk.dispatch_table.DestroySemaphore(device, semaphore, NULL);

   VkSemaphoreTypeCreateInfo binary_type = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_BINARY,
      .initialValue = 42,
   };
   create_info.pNext = &binary_type;
   semaphore = VK_NULL_HANDLE;
   assert(fixture->device.vk.dispatch_table.CreateSemaphore(
             device, &create_info, NULL, &semaphore) == VK_SUCCESS);
   semaphore_obj = vk_semaphore_from_handle(semaphore);
   assert(vk_sync_wait(&fixture->device.vk, &semaphore_obj->permanent, 0,
                       VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   fixture->device.vk.dispatch_table.DestroySemaphore(device, semaphore, NULL);

   VkSemaphoreTypeCreateInfo timeline_type = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 1,
   };
   create_info.pNext = &timeline_type;
   assert(fixture->device.vk.dispatch_table.CreateSemaphore(
             device, &create_info, NULL, &semaphore) ==
          VK_ERROR_FEATURE_NOT_PRESENT);

   VkExportSemaphoreCreateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
   };
   create_info.pNext = &export_info;
   assert(fixture->device.vk.dispatch_table.CreateSemaphore(
             device, &create_info, NULL, &semaphore) ==
          VK_ERROR_INVALID_EXTERNAL_HANDLE);

   fixture->fail_allocation = true;
   create_info.pNext = NULL;
   assert(fixture->device.vk.dispatch_table.CreateSemaphore(
             device, &create_info, &fixture->allocation_callbacks, &semaphore) ==
          VK_ERROR_OUT_OF_HOST_MEMORY);
   fixture->fail_allocation = false;
}

static void
test_common_fences_and_queue_idle(struct runtime_fixture *fixture)
{
   VkDevice device = vk_device_to_handle(&fixture->device.vk);
   VkFenceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   VkFence fence = VK_NULL_HANDLE;
   assert(fixture->device.vk.dispatch_table.CreateFence(
             device, &create_info, NULL, &fence) == VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.GetFenceStatus(device, fence) ==
          VK_NOT_READY);
   assert(fixture->device.vk.dispatch_table.ResetFences(device, 1, &fence) ==
          VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.WaitForFences(
             device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT);

   VkQueue queue = vk_queue_to_handle(&fixture->queue);
   assert(fixture->device.vk.dispatch_table.QueueWaitIdle(queue) == VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.GetFenceStatus(device, fence) ==
          VK_NOT_READY);
   assert(fixture->device.vk.dispatch_table.QueueSubmit(queue, 0, NULL, fence) ==
          VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.GetFenceStatus(device, fence) ==
          VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.ResetFences(device, 1, &fence) ==
          VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.GetFenceStatus(device, fence) ==
          VK_NOT_READY);
   fixture->device.vk.dispatch_table.DestroyFence(device, fence, NULL);
   VkFence fences[2] = {0};
   create_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
   assert(fixture->device.vk.dispatch_table.CreateFence(device, &create_info, NULL, &fences[0]) == VK_SUCCESS);
   create_info.flags = 0;
   assert(fixture->device.vk.dispatch_table.CreateFence(device, &create_info, NULL, &fences[1]) == VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.WaitForFences(device, 2, fences, VK_FALSE, 0) == VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.WaitForFences(device, 2, fences, VK_TRUE, 0) == VK_TIMEOUT);
   assert(fixture->device.vk.dispatch_table.QueueSubmit(queue, 0, NULL, fences[1]) == VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.WaitForFences(device, 2, fences, VK_TRUE, 0) == VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.ResetFences(device, 2, fences) == VK_SUCCESS);
   assert(fixture->device.vk.dispatch_table.WaitForFences(device, 2, fences, VK_FALSE, 0) == VK_TIMEOUT);
   for (unsigned i = 0; i < 2; i++)
      fixture->device.vk.dispatch_table.DestroyFence(device, fences[i], NULL);
}

static void
test_device_capability_rejection(struct runtime_fixture *fixture)
{
   struct vk_device rejected;
   struct vk_device_dispatch_table dispatch = {0};
   VkPhysicalDeviceFeatures features = {.geometryShader = VK_TRUE};
   VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .pEnabledFeatures = &features};
   assert(vk_device_init(&rejected, &fixture->physical_device.vk, &dispatch,
                         &info, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
   info.pEnabledFeatures = NULL;
   const char *extension = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
   info.enabledExtensionCount = 1;
   info.ppEnabledExtensionNames = &extension;
   assert(vk_device_init(&rejected, &fixture->physical_device.vk, &dispatch,
                         &info, NULL) == VK_ERROR_EXTENSION_NOT_PRESENT);
}

int
main(void)
{
   struct runtime_fixture fixture;
   fixture_init(&fixture);
   test_device_capability_rejection(&fixture);
   test_binary_semaphore_lifecycle(&fixture);
   test_common_fences_and_queue_idle(&fixture);
   fixture_finish(&fixture);
   return 0;
}
