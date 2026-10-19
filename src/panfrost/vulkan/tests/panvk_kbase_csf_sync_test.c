#include "panvk_kbase_csf_sync.h"
#include "panvk_kbase_sync.h"
#include "kmod/kbase_csf.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct kbase_csf_queue {
   unsigned refs;
   uint64_t published;
   uint64_t completed;
   bool lost;
};

void __wrap_kbase_csf_queue_ref(struct kbase_csf_queue *queue);
void __wrap_kbase_csf_queue_unref(struct kbase_csf_queue *queue);
uint64_t __wrap_kbase_csf_queue_published_target(struct kbase_csf_queue *queue);
int __wrap_kbase_csf_wait(struct kbase_csf_queue *const *queues, uint32_t count,
                         const uint64_t *targets, uint64_t deadline);

void
__wrap_kbase_csf_queue_ref(struct kbase_csf_queue *queue)
{
   queue->refs++;
}

void
__wrap_kbase_csf_queue_unref(struct kbase_csf_queue *queue)
{
   assert(queue->refs > 0);
   queue->refs--;
}

uint64_t
__wrap_kbase_csf_queue_published_target(struct kbase_csf_queue *queue)
{
   return queue->published;
}

int
__wrap_kbase_csf_wait(struct kbase_csf_queue *const *queues, uint32_t count,
                      const uint64_t *targets, UNUSED uint64_t deadline)
{
   bool complete = true;
   for (uint32_t i = 0; i < count; i++) {
      if (queues[i]->lost) {
         errno = EIO;
         return -1;
      }
      if (queues[i]->completed < targets[i])
         complete = false;
   }
   return complete ? 1 : 0;
}

int
main(void)
{
   struct kbase_csf_queue a = {.refs = 1, .published = 3, .completed = 0};
   struct kbase_csf_queue b = {.refs = 1, .published = 5, .completed = 5};
   struct kbase_csf_queue *queues[] = {&a, &b};
   struct vk_sync *sync = calloc(1, panvk_kbase_sync_type.size);
   struct vk_sync *moved = calloc(1, panvk_kbase_sync_type.size);
   assert(sync && moved);
   assert(vk_sync_init(NULL, sync, &panvk_kbase_sync_type, 0, 0) == VK_SUCCESS);
   assert(vk_sync_init(NULL, moved, &panvk_kbase_sync_type, 0, 0) == VK_SUCCESS);
   assert(panvk_kbase_csf_sync_attach(sync, queues, 2) == VK_SUCCESS);
   assert(a.refs == 2 && b.refs == 2);
   assert(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   assert(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   a.published = 9;
   a.completed = 3;
   assert(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   assert(vk_sync_move(NULL, moved, sync) == VK_SUCCESS);
   assert(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   assert(a.refs == 2 && b.refs == 2);
   assert(vk_sync_reset(NULL, moved) == VK_SUCCESS);
   assert(a.refs == 1 && b.refs == 1);
   assert(panvk_kbase_csf_sync_attach(sync, queues, 2) == VK_SUCCESS);
   b.lost = true;
   assert(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_ERROR_DEVICE_LOST);
   assert(vk_sync_reset(NULL, sync) == VK_SUCCESS);
   assert(a.refs == 1 && b.refs == 1);
   vk_sync_finish(NULL, sync);
   vk_sync_finish(NULL, moved);
   free(sync);
   free(moved);
   return 0;
}
