#ifndef PANVK_KBASE_CSF_SUBMIT_H
#define PANVK_KBASE_CSF_SUBMIT_H

#include "panvk_kbase_csf_encode.h"
#include "vk_sync.h"

struct kbase_csf_queue;

struct panvk_kbase_csf_submit_item {
   struct kbase_csf_queue *queue;
   uint64_t target;
   panvk_kbase_csf_encode_func encode;
   struct panvk_kbase_csf_encode_info info;
};

VkResult panvk_kbase_csf_submit(
   const struct panvk_kbase_csf_submit_item *items, uint32_t count,
   struct vk_sync *const *signals, uint32_t signal_count);

#endif
