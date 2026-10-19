/*
 * DEAD CODE - no production callers.
 *
 * Provides the completion half of the unused panvk_kbase_csf_submit() wrapper:
 * a snapshot of queue/target pairs plus the vk_sync completion plumbing. Only
 * panvk_kbase_csf_submit_test and panvk_kbase_sync_test exercise it.
 *
 * Production waits through kbase_csf_wait() in csf/panvk_kbase_compute_queue.inc.
 * Kept, not deleted, because this is the structure an asynchronous submission
 * would build on; see the header comment in panvk_kbase_csf_submit.c.
 */
#include "panvk_kbase_csf_sync.h"
#include "panvk_kbase_sync.h"

#include <errno.h>
#include <stdlib.h>

#include "kmod/kbase_csf.h"
#include "util/u_atomic.h"

struct panvk_kbase_csf_snapshot {
   int32_t refs;
   uint32_t count;
   struct kbase_csf_queue *queues[];
};

static void
snapshot_retain(void *data)
{
   struct panvk_kbase_csf_snapshot *snapshot = data;
   p_atomic_inc(&snapshot->refs);
}

static void
snapshot_release(void *data)
{
   struct panvk_kbase_csf_snapshot *snapshot = data;
   if (!p_atomic_dec_zero(&snapshot->refs))
      return;
   for (uint32_t i = 0; i < snapshot->count; i++)
      kbase_csf_queue_unref(snapshot->queues[i]);
   free(snapshot);
}

static VkResult
snapshot_wait(void *data, uint32_t count, const uint64_t *targets,
                uint64_t deadline)
{
   struct panvk_kbase_csf_snapshot *snapshot = data;
   if (snapshot->count != count)
      return VK_ERROR_UNKNOWN;
   int ret = kbase_csf_wait(snapshot->queues, count, targets, deadline);
   if (ret > 0)
      return VK_SUCCESS;
   if (ret == 0)
      return VK_TIMEOUT;
   return VK_ERROR_DEVICE_LOST;
}

VkResult
panvk_kbase_csf_sync_prepare(struct kbase_csf_queue *const *queues,
                              uint32_t count, const uint64_t *targets,
                              struct panvk_kbase_completion **out)
{
   if (out)
      *out = NULL;
   if (!out || !queues || !targets || !count || count > 64)
      return VK_ERROR_UNKNOWN;
   for (uint32_t i = 0; i < count; i++) {
      if (!queues[i])
         return VK_ERROR_UNKNOWN;
      for (uint32_t j = 0; j < i; j++) {
         if (queues[i] == queues[j])
            return VK_ERROR_UNKNOWN;
      }
   }
   struct panvk_kbase_csf_snapshot *snapshot =
      calloc(1, sizeof(*snapshot) + count * sizeof(*queues));
   if (!snapshot)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   snapshot->refs = 1;
   snapshot->count = count;
   for (uint32_t i = 0; i < count; i++) {
      snapshot->queues[i] = queues[i];
      kbase_csf_queue_ref(queues[i]);
   }
   const struct panvk_kbase_completion_ops ops = {
      .wait = snapshot_wait,
      .retain = snapshot_retain,
      .release = snapshot_release,
   };
   VkResult result =
      panvk_kbase_completion_prepare(&ops, snapshot, count, targets, out);
   snapshot_release(snapshot);
   return result;
}

VkResult
panvk_kbase_csf_sync_attach(struct vk_sync *sync,
                             struct kbase_csf_queue *const *queues,
                             uint32_t count)
{
   if (!sync || sync->type != &panvk_kbase_sync_type || !queues ||
       !count || count > 64)
      return VK_ERROR_UNKNOWN;
   uint64_t targets[64];
   for (uint32_t i = 0; i < count; i++) {
      if (!queues[i])
         return VK_ERROR_UNKNOWN;
      targets[i] = kbase_csf_queue_published_target(queues[i]);
   }
   struct panvk_kbase_completion *completion = NULL;
   VkResult result = panvk_kbase_csf_sync_prepare(queues, count, targets, &completion);
   if (result != VK_SUCCESS)
      return result;
   panvk_kbase_completion_commit_many(1, &sync, &completion);
   return VK_SUCCESS;
}
