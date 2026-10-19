/*
 * Copyright © 2021 Collabora Ltd.
 * Copyright © 2026 Google LLC
 * Copyright © 2026 Arm Ltd.
 *
 * Derived from tu_device.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "panvk_kbase_sync.h"

#include <stdlib.h>
#include <string.h>

#include "util/cnd_monotonic.h"
#include "util/os_time.h"
#include "util/timespec.h"

#define KBASE_SYNC_WAIT_SLICE_NS 1000000ull
#define KBASE_SYNC_MAX_TARGETS 64

struct panvk_kbase_completion {
   struct panvk_kbase_completion_ops ops;
   void *data;
   uint32_t refcount;
   uint32_t target_count;
   bool resolving;
   VkResult result;
   uint64_t targets[];
};

struct panvk_kbase_sync {
   struct vk_sync base;
   struct panvk_kbase_completion *completion;
   uint64_t generation;
   bool signaled;
};

static struct {
   once_flag once;
   mtx_t mutex;
   struct u_cnd_monotonic cond;
   uint64_t sequence;
   int status;
} sync_domain = {
   .once = ONCE_FLAG_INIT,
   .status = thrd_error,
};

static void
kbase_sync_domain_init(void)
{
   int ret = mtx_init(&sync_domain.mutex, mtx_plain);
   if (ret != thrd_success)
      return;
   ret = u_cnd_monotonic_init(&sync_domain.cond);
   if (ret != thrd_success) {
      mtx_destroy(&sync_domain.mutex);
      return;
   }
   sync_domain.status = thrd_success;
}

static struct panvk_kbase_sync *
to_kbase_sync(struct vk_sync *sync)
{
   assert(sync->type == &panvk_kbase_sync_type);
   return container_of(sync, struct panvk_kbase_sync, base);
}

static void
kbase_sync_notify_locked(void)
{
   sync_domain.sequence++;
   u_cnd_monotonic_broadcast(&sync_domain.cond);
}

static struct panvk_kbase_completion *
kbase_completion_unref_locked(struct panvk_kbase_completion *completion)
{
   if (!completion)
      return NULL;
   assert(completion->refcount > 0);
   return --completion->refcount == 0 ? completion : NULL;
}

static void
kbase_completion_free(struct panvk_kbase_completion *completion)
{
   if (!completion)
      return;
   completion->ops.release(completion->data);
   free(completion);
}

static VkResult
kbase_sync_init(UNUSED struct vk_device *device, struct vk_sync *sync,
                 uint64_t initial_value)
{
   call_once(&sync_domain.once, kbase_sync_domain_init);
   if (sync_domain.status != thrd_success)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct panvk_kbase_sync *ks = to_kbase_sync(sync);
   ks->completion = NULL;
   ks->generation = 0;
   ks->signaled = initial_value != 0;
   return VK_SUCCESS;
}

static void
kbase_sync_finish(UNUSED struct vk_device *device, struct vk_sync *sync)
{
   struct panvk_kbase_sync *ks = to_kbase_sync(sync);
   mtx_lock(&sync_domain.mutex);
   struct panvk_kbase_completion *dead =
      kbase_completion_unref_locked(ks->completion);
   ks->completion = NULL;
   ks->signaled = false;
   ks->generation++;
   kbase_sync_notify_locked();
   mtx_unlock(&sync_domain.mutex);
   kbase_completion_free(dead);
}

static VkResult
kbase_sync_replace(struct vk_sync *sync, bool signaled,
                     struct panvk_kbase_completion *completion)
{
   struct panvk_kbase_sync *ks = to_kbase_sync(sync);
   mtx_lock(&sync_domain.mutex);
   struct panvk_kbase_completion *dead =
      kbase_completion_unref_locked(ks->completion);
   ks->completion = completion;
   ks->signaled = signaled;
   ks->generation++;
   kbase_sync_notify_locked();
   mtx_unlock(&sync_domain.mutex);
   kbase_completion_free(dead);
   return VK_SUCCESS;
}

static VkResult
kbase_sync_signal(UNUSED struct vk_device *device, struct vk_sync *sync,
                   UNUSED uint64_t value)
{
   return kbase_sync_replace(sync, true, NULL);
}

static VkResult
kbase_sync_reset(UNUSED struct vk_device *device, struct vk_sync *sync)
{
   return kbase_sync_replace(sync, false, NULL);
}

VkResult
panvk_kbase_completion_prepare(const struct panvk_kbase_completion_ops *ops,
                                void *data, uint32_t target_count,
                                const uint64_t *targets,
                                struct panvk_kbase_completion **out)
{
   if (out)
      *out = NULL;
   if (!out || !ops || !ops->wait || !ops->retain || !ops->release || !targets ||
       target_count == 0 || target_count > KBASE_SYNC_MAX_TARGETS)
      return VK_ERROR_UNKNOWN;
   call_once(&sync_domain.once, kbase_sync_domain_init);
   if (sync_domain.status != thrd_success)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   struct panvk_kbase_completion *completion =
      calloc(1, sizeof(*completion) + target_count * sizeof(*targets));
   if (!completion)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   completion->ops = *ops;
   completion->data = data;
   completion->refcount = 1;
   completion->target_count = target_count;
   completion->result = VK_NOT_READY;
   memcpy(completion->targets, targets, target_count * sizeof(*targets));
   completion->ops.retain(data);
   *out = completion;
   return VK_SUCCESS;
}

void
panvk_kbase_completion_ref(struct panvk_kbase_completion *completion)
{
   assert(completion);
   mtx_lock(&sync_domain.mutex);
   assert(completion->refcount > 0 && completion->refcount != UINT32_MAX);
   completion->refcount++;
   mtx_unlock(&sync_domain.mutex);
}

void
panvk_kbase_completion_unref(struct panvk_kbase_completion *completion)
{
   if (!completion)
      return;
   mtx_lock(&sync_domain.mutex);
   struct panvk_kbase_completion *dead =
      kbase_completion_unref_locked(completion);
   mtx_unlock(&sync_domain.mutex);
   kbase_completion_free(dead);
}

void
panvk_kbase_completion_commit_many(uint32_t count, struct vk_sync *const *syncs,
                                   struct panvk_kbase_completion **completions)
{
   assert(count <= KBASE_SYNC_MAX_TARGETS);
   struct panvk_kbase_completion *dead[KBASE_SYNC_MAX_TARGETS];
   mtx_lock(&sync_domain.mutex);
   for (uint32_t i = 0; i < count; i++) {
      assert(syncs[i] && syncs[i]->type == &panvk_kbase_sync_type);
      assert(completions[i]);
      for (uint32_t j = 0; j < i; j++)
         assert(syncs[j] != syncs[i]);
      struct panvk_kbase_sync *ks = to_kbase_sync(syncs[i]);
      dead[i] = kbase_completion_unref_locked(ks->completion);
      ks->completion = completions[i];
      ks->signaled = false;
      ks->generation++;
      completions[i] = NULL;
   }
   if (count)
      kbase_sync_notify_locked();
   mtx_unlock(&sync_domain.mutex);
   for (uint32_t i = 0; i < count; i++)
      kbase_completion_free(dead[i]);
}

VkResult
panvk_kbase_sync_set_pending(struct vk_sync *sync,
                              const struct panvk_kbase_completion_ops *ops,
                              void *data, uint32_t target_count,
                              const uint64_t *targets)
{
   if (!sync || sync->type != &panvk_kbase_sync_type)
      return VK_ERROR_UNKNOWN;
   struct panvk_kbase_completion *completion = NULL;
   VkResult result =
      panvk_kbase_completion_prepare(ops, data, target_count, targets, &completion);
   if (result != VK_SUCCESS)
      return result;
   panvk_kbase_completion_commit_many(1, &sync, &completion);
   return VK_SUCCESS;
}

static VkResult
kbase_sync_move(UNUSED struct vk_device *device, struct vk_sync *dst,
                 struct vk_sync *src)
{
   if (dst == src)
      return VK_SUCCESS;
   struct panvk_kbase_sync *to = to_kbase_sync(dst);
   struct panvk_kbase_sync *from = to_kbase_sync(src);
   mtx_lock(&sync_domain.mutex);
   struct panvk_kbase_completion *dead =
      kbase_completion_unref_locked(to->completion);
   to->completion = from->completion;
   to->signaled = from->signaled;
   to->generation++;
   from->completion = NULL;
   from->signaled = false;
   from->generation++;
   kbase_sync_notify_locked();
   mtx_unlock(&sync_domain.mutex);
   kbase_completion_free(dead);
   return VK_SUCCESS;
}

static uint64_t
kbase_sync_slice(uint64_t deadline)
{
   if (deadline == 0)
      return 0;
   uint64_t now = os_time_get_nano();
   uint64_t slice = now > UINT64_MAX - KBASE_SYNC_WAIT_SLICE_NS ? UINT64_MAX :
                       now + KBASE_SYNC_WAIT_SLICE_NS;
   return MIN2(deadline, slice);
}

static VkResult
kbase_sync_probe(struct vk_sync *sync, enum vk_sync_wait_flags flags,
                  uint64_t deadline, bool retry_stale)
{
   struct panvk_kbase_sync *ks = to_kbase_sync(sync);
   mtx_lock(&sync_domain.mutex);
   struct panvk_kbase_completion *completion = ks->completion;
   if (ks->signaled) {
      mtx_unlock(&sync_domain.mutex);
      return VK_SUCCESS;
   }
   if (!completion) {
      mtx_unlock(&sync_domain.mutex);
      return VK_NOT_READY;
   }
   if (completion->result != VK_NOT_READY) {
      VkResult result = completion->result;
      mtx_unlock(&sync_domain.mutex);
      return result;
   }
   if (flags & VK_SYNC_WAIT_PENDING) {
      mtx_unlock(&sync_domain.mutex);
      return VK_SUCCESS;
   }
   if (completion->resolving) {
      mtx_unlock(&sync_domain.mutex);
      return VK_NOT_READY;
   }

   uint64_t generation = ks->generation;
   completion->refcount++;
   completion->resolving = true;
   mtx_unlock(&sync_domain.mutex);

   VkResult result = completion->ops.wait(completion->data,
                                          completion->target_count,
                                          completion->targets, deadline);
   if (result != VK_SUCCESS && result != VK_TIMEOUT && result >= 0)
      result = VK_ERROR_UNKNOWN;

   mtx_lock(&sync_domain.mutex);
   completion->resolving = false;
   if (result != VK_TIMEOUT) {
      completion->result = result;
      kbase_sync_notify_locked();
   }
   bool stale = ks->completion != completion || ks->generation != generation;
   if (stale)
      result = VK_NOT_READY;
   else if (result == VK_TIMEOUT)
      result = VK_NOT_READY;
   struct panvk_kbase_completion *dead =
      kbase_completion_unref_locked(completion);
   mtx_unlock(&sync_domain.mutex);
   kbase_completion_free(dead);
   if (stale && retry_stale)
      return kbase_sync_probe(sync, flags, 0, false);
   return result;
}

static uint64_t
kbase_sync_sequence(void)
{
   mtx_lock(&sync_domain.mutex);
   uint64_t sequence = sync_domain.sequence;
   mtx_unlock(&sync_domain.mutex);
   return sequence;
}

static VkResult
kbase_sync_pause(uint64_t sequence, uint64_t deadline)
{
   mtx_lock(&sync_domain.mutex);
   if (sync_domain.sequence == sequence) {
      struct timespec timeout;
      timespec_from_nsec(&timeout, kbase_sync_slice(deadline));
      int ret = u_cnd_monotonic_timedwait(&sync_domain.cond,
                                          &sync_domain.mutex, &timeout);
      if (ret != thrd_success && ret != thrd_timedout) {
         mtx_unlock(&sync_domain.mutex);
         return VK_ERROR_UNKNOWN;
      }
   }
   mtx_unlock(&sync_domain.mutex);
   return VK_SUCCESS;
}

static bool
kbase_sync_expired(uint64_t deadline)
{
   return deadline != UINT64_MAX &&
          (deadline == 0 || (uint64_t)os_time_get_nano() >= deadline);
}

static VkResult
kbase_sync_wait(UNUSED struct vk_device *device, struct vk_sync *sync,
                 UNUSED uint64_t value, enum vk_sync_wait_flags flags,
                 uint64_t deadline)
{
   if (flags & ~(VK_SYNC_WAIT_PENDING | VK_SYNC_WAIT_ANY))
      return VK_ERROR_UNKNOWN;
   while (true) {
      uint64_t sequence = kbase_sync_sequence();
      VkResult result =
         kbase_sync_probe(sync, flags, kbase_sync_slice(deadline), true);
      if (result != VK_NOT_READY)
         return result;
      if (kbase_sync_expired(deadline))
         return VK_TIMEOUT;
      result = kbase_sync_pause(sequence, deadline);
      if (result != VK_SUCCESS)
         return result;
   }
}

static VkResult
kbase_sync_wait_many(struct vk_device *device, uint32_t count,
                      const struct vk_sync_wait *waits,
                      enum vk_sync_wait_flags flags, uint64_t deadline)
{
   if (count == 0)
      return VK_SUCCESS;
   if (!waits || (flags & ~(VK_SYNC_WAIT_PENDING | VK_SYNC_WAIT_ANY)))
      return VK_ERROR_UNKNOWN;
   if (!(flags & VK_SYNC_WAIT_ANY)) {
      for (uint32_t i = 0; i < count; i++) {
         VkResult result = kbase_sync_wait(device, waits[i].sync,
                                            waits[i].wait_value, flags, deadline);
         if (result != VK_SUCCESS)
            return result;
      }
      return VK_SUCCESS;
   }

   while (true) {
      uint64_t sequence = kbase_sync_sequence();
      for (uint32_t i = 0; i < count; i++) {
         VkResult result = kbase_sync_probe(waits[i].sync, flags, 0, true);
         if (result != VK_NOT_READY)
            return result;
      }
      if (kbase_sync_expired(deadline))
         return VK_TIMEOUT;
      VkResult result = kbase_sync_pause(sequence, deadline);
      if (result != VK_SUCCESS)
         return result;
   }
}

const struct vk_sync_type panvk_kbase_sync_type = {
   .size = sizeof(struct panvk_kbase_sync),
   .features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET | VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_PENDING | VK_SYNC_FEATURE_WAIT_ANY,
   .init = kbase_sync_init,
   .finish = kbase_sync_finish,
   .signal = kbase_sync_signal,
   .reset = kbase_sync_reset,
   .move = kbase_sync_move,
   .wait = kbase_sync_wait,
   .wait_many = kbase_sync_wait_many,
};
