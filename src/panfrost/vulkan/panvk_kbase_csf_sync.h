#ifndef PANVK_KBASE_CSF_SYNC_H
#define PANVK_KBASE_CSF_SYNC_H

#include "vk_sync.h"

struct kbase_csf_queue;
struct panvk_kbase_completion;

VkResult panvk_kbase_csf_sync_prepare(
   struct kbase_csf_queue *const *queues, uint32_t count,
   const uint64_t *targets, struct panvk_kbase_completion **out);

VkResult panvk_kbase_csf_sync_attach(struct vk_sync *sync,
                                     struct kbase_csf_queue *const *queues,
                                     uint32_t count);

#endif
