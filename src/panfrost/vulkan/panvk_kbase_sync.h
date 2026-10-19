#ifndef PANVK_KBASE_SYNC_H
#define PANVK_KBASE_SYNC_H

#include "vk_sync.h"

struct panvk_kbase_completion_ops {
   VkResult (*wait)(void *data, uint32_t target_count, const uint64_t *targets,
                    uint64_t abs_timeout_ns);
   void (*retain)(void *data);
   void (*release)(void *data);
};

extern const struct vk_sync_type panvk_kbase_sync_type;

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateSemaphore(VkDevice device,
                     const VkSemaphoreCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkSemaphore *pSemaphore);

struct panvk_kbase_completion;

VkResult panvk_kbase_completion_prepare(
   const struct panvk_kbase_completion_ops *ops, void *data,
   uint32_t target_count, const uint64_t *targets,
   struct panvk_kbase_completion **out);
void panvk_kbase_completion_ref(struct panvk_kbase_completion *completion);
void panvk_kbase_completion_unref(struct panvk_kbase_completion *completion);
void panvk_kbase_completion_commit_many(
   uint32_t count, struct vk_sync *const *syncs,
   struct panvk_kbase_completion **completions);

VkResult panvk_kbase_sync_set_pending(
   struct vk_sync *sync, const struct panvk_kbase_completion_ops *ops,
   void *data, uint32_t target_count, const uint64_t *targets);

#endif
