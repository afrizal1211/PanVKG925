#include "panvk_kbase_csf_submit.h"
#include "panvk_kbase_csf_sync.h"
#include "panvk_kbase_sync.h"
#include "kmod/kbase_csf.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

struct kbase_csf_queue {
   unsigned refs;
   uint64_t seqno_va;
   uint64_t published;
   uint64_t completed;
};

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
void __wrap_kbase_csf_queue_ref(struct kbase_csf_queue *queue);
void __wrap_kbase_csf_queue_unref(struct kbase_csf_queue *queue);
uint64_t __wrap_kbase_csf_queue_seqno_va(struct kbase_csf_queue *queue);
uint64_t __wrap_kbase_csf_queue_published_target(struct kbase_csf_queue *queue);
int __wrap_kbase_csf_wait(struct kbase_csf_queue *const *queues, uint32_t count,
                         const uint64_t *targets, uint64_t deadline);
int __wrap_kbase_csf_submit(const struct kbase_csf_submit_entry *entries,
                           uint32_t count);

static unsigned calloc_calls;
static unsigned fail_calloc_at;
static bool forbid_calloc;
static unsigned submit_calls;
static int submit_errno;
static unsigned published_target_calls;
static struct vk_sync *submit_signals[2];
static uint32_t submit_signal_count;

void *
__wrap_calloc(size_t count, size_t size)
{
   calloc_calls++;
   assert(!forbid_calloc);
   if (fail_calloc_at && calloc_calls == fail_calloc_at)
      return NULL;
   return __real_calloc(count, size);
}

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
__wrap_kbase_csf_queue_seqno_va(struct kbase_csf_queue *queue)
{
   return queue->seqno_va;
}

uint64_t
__wrap_kbase_csf_queue_published_target(struct kbase_csf_queue *queue)
{
   published_target_calls++;
   return queue->published;
}

int
__wrap_kbase_csf_wait(struct kbase_csf_queue *const *queues, uint32_t count,
                      const uint64_t *targets, UNUSED uint64_t deadline)
{
   for (uint32_t i = 0; i < count; i++) {
      if (queues[i]->completed < targets[i])
         return 0;
   }
   return 1;
}

int
__wrap_kbase_csf_submit(const struct kbase_csf_submit_entry *entries,
                        uint32_t count)
{
   submit_calls++;
   assert(count == 2);
   assert(submit_signal_count == 2);
   for (uint32_t i = 0; i < submit_signal_count; i++)
      assert(vk_sync_wait(NULL, submit_signals[i], 0,
                          VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   assert(entries[0].target == 11 && entries[1].target == 22);
   assert(entries[0].size == sizeof(uint64_t));
   assert(entries[1].size == sizeof(uint64_t));
   assert(*(const uint64_t *)entries[0].commands == 101);
   assert(*(const uint64_t *)entries[1].commands == 202);
   if (submit_errno) {
      errno = submit_errno;
      forbid_calloc = true;
      return -1;
   }
   entries[0].queue->published = entries[0].target;
   entries[1].queue->published = entries[1].target;
   forbid_calloc = true;
   return 0;
}

static bool
encode_test(const struct panvk_kbase_csf_encode_info *info, uint64_t *commands,
           uint32_t capacity_bytes, uint32_t *size_bytes)
{
   if (info->flush_id == UINT32_MAX)
      return false;
   if (capacity_bytes < sizeof(*commands))
      return false;
   *commands = info->flush_id;
   *size_bytes = sizeof(*commands);
   return true;
}

static struct vk_sync *
new_sync(void)
{
   struct vk_sync *sync = calloc(1, panvk_kbase_sync_type.size);
   assert(sync);
   assert(vk_sync_init(NULL, sync, &panvk_kbase_sync_type, 0, 0) == VK_SUCCESS);
   return sync;
}

static void
free_sync(struct vk_sync *sync)
{
   vk_sync_finish(NULL, sync);
   free(sync);
}

static void
check_unsubmitted(struct vk_sync *const *signals, uint32_t count,
                  const struct kbase_csf_queue *a,
                  const struct kbase_csf_queue *b)
{
   for (uint32_t i = 0; i < count; i++)
      assert(vk_sync_wait(NULL, signals[i], 0,
                          VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   assert(a->refs == 1 && b->refs == 1);
}

int
main(void)
{
   struct kbase_csf_queue a = {
      .refs = 1, .seqno_va = 0x1000, .published = 3,
   };
   struct kbase_csf_queue b = {
      .refs = 1, .seqno_va = 0x2000, .published = 5,
   };
   struct panvk_kbase_csf_submit_item items[2] = {
      {.queue = &a, .target = 11, .encode = encode_test,
       .info = {.flush_id = 101, .seqno_address = 0x1000}},
      {.queue = &b, .target = 22, .encode = encode_test,
       .info = {.flush_id = 202, .seqno_address = 0x2000}},
   };
   struct vk_sync *s0 = new_sync();
   struct vk_sync *s1 = new_sync();
   struct vk_sync *signals[] = {s0, s1};
   submit_signals[0] = s0;
   submit_signals[1] = s1;
   submit_signal_count = 2;

   assert(panvk_kbase_csf_submit(items, 2, signals, 2) == VK_SUCCESS);
   assert(submit_calls == 1);
   assert(published_target_calls == 0);
   assert(forbid_calloc);
   assert(a.refs == 2 && b.refs == 2);
   assert(vk_sync_wait(NULL, s0, 0, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   assert(vk_sync_wait(NULL, s1, 0, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   assert(vk_sync_wait(NULL, s0, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   a.completed = 11;
   b.completed = 22;
   assert(vk_sync_wait(NULL, s0, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   assert(vk_sync_wait(NULL, s1, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   assert(a.refs == 2 && b.refs == 2);
   forbid_calloc = false;
   free_sync(s0);
   free_sync(s1);
   assert(a.refs == 1 && b.refs == 1);

   s0 = new_sync();
   s1 = new_sync();
   signals[0] = s0;
   signals[1] = s1;
   items[1].info.flush_id = UINT32_MAX;
   submit_signals[0] = s0;
   submit_signals[1] = s1;
   submit_calls = 0;
   forbid_calloc = false;
   assert(panvk_kbase_csf_submit(items, 2, signals, 2) == VK_ERROR_UNKNOWN);
   assert(submit_calls == 0);
   check_unsubmitted(signals, 2, &a, &b);

   items[1].info.flush_id = 202;
   for (unsigned fail_at = 1; fail_at <= 3; fail_at++) {
      calloc_calls = 0;
      fail_calloc_at = fail_at;
      assert(panvk_kbase_csf_submit(items, 2, signals, 2) ==
             VK_ERROR_OUT_OF_HOST_MEMORY);
      assert(calloc_calls == fail_at);
      assert(submit_calls == 0);
      check_unsubmitted(signals, 2, &a, &b);
   }
   fail_calloc_at = 0;
   calloc_calls = 0;

   submit_errno = EINVAL;
   assert(panvk_kbase_csf_submit(items, 2, signals, 2) == VK_ERROR_UNKNOWN);
   assert(submit_calls == 1);
   assert(a.published == 11 && b.published == 22);
   check_unsubmitted(signals, 2, &a, &b);
   forbid_calloc = false;

   submit_errno = EIO;
   submit_calls = 0;
   assert(panvk_kbase_csf_submit(items, 2, signals, 2) == VK_ERROR_DEVICE_LOST);
   assert(submit_calls == 1);
   check_unsubmitted(signals, 2, &a, &b);
   forbid_calloc = false;
   submit_errno = 0;

   free_sync(s0);
   free_sync(s1);
   return 0;
}
