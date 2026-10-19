#include "panvk_kbase_sync.h"
#include "panvk_physical_device.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_semaphore.h"
#include "vk_util.h"

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateSemaphore(VkDevice _device,
                      const VkSemaphoreCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkSemaphore *pSemaphore)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->physical);

   if (!panvk_physical_device_uses_kbase(physical_device))
      return vk_common_CreateSemaphore(_device, pCreateInfo, pAllocator,
                                       pSemaphore);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
   const VkSemaphoreTypeCreateInfo *type_info =
      vk_find_struct_const(pCreateInfo->pNext, SEMAPHORE_TYPE_CREATE_INFO);
   if (type_info && type_info->semaphoreType != VK_SEMAPHORE_TYPE_BINARY)
      return vk_error(device, VK_ERROR_FEATURE_NOT_PRESENT);

   const VkExportSemaphoreCreateInfo *export_info =
      vk_find_struct_const(pCreateInfo->pNext, EXPORT_SEMAPHORE_CREATE_INFO);
   if (export_info && export_info->handleTypes != 0)
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   const size_t size = offsetof(struct vk_semaphore, permanent) +
                       panvk_kbase_sync_type.size;
   struct vk_semaphore *semaphore =
      vk_object_zalloc(device, pAllocator, size, VK_OBJECT_TYPE_SEMAPHORE);
   if (!semaphore)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   semaphore->type = VK_SEMAPHORE_TYPE_BINARY;
   VkResult result = vk_sync_init(device, &semaphore->permanent,
                                  &panvk_kbase_sync_type, 0, 0);
   if (result != VK_SUCCESS) {
      vk_object_free(device, pAllocator, semaphore);
      return result;
   }

   *pSemaphore = vk_semaphore_to_handle(semaphore);
   return VK_SUCCESS;
}
