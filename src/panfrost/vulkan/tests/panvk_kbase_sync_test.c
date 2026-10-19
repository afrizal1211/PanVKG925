#include "panvk_kbase_sync.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"
#include "util/os_time.h"
#include "util/cnd_monotonic.h"
#include "util/timespec.h"
#include "vk_device.h"
#include "vk_physical_device.h"

#define CHECK(c) do { if (!(c)) { \
   fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); \
   abort(); \
} } while (0)
#define TEST_TIMEOUT_NS 1000000000ull
#define TEST_DEADLINE (os_time_get_nano() + TEST_TIMEOUT_NS)

struct test_context {
   mtx_t mutex;
   struct u_cnd_monotonic cond;
   atomic_int refs;
   VkResult result;
   uint32_t calls;
   uint64_t first_deadline;
   uint64_t last_deadline;
   struct vk_sync *signal_on_wait;
   uint32_t expected_count;
   uint64_t expected_targets[64];
   bool gate;
   bool entered;
   bool proceed;
};

struct test_sync {
   struct vk_sync *sync;
};

struct wait_thread_args {
   struct vk_device *device;
   struct vk_sync *sync;
   VkResult result;
   enum vk_sync_wait_flags flags;
   uint64_t deadline;
};

static void
context_init(struct test_context *ctx, VkResult result)
{
   memset(ctx, 0, sizeof(*ctx));
   CHECK(mtx_init(&ctx->mutex, mtx_plain) == thrd_success);
   CHECK(u_cnd_monotonic_init(&ctx->cond) == thrd_success);
   atomic_init(&ctx->refs, 1);
   ctx->result = result;
}

static void
context_finish(struct test_context *ctx)
{
   CHECK(atomic_load(&ctx->refs) == 1);
   u_cnd_monotonic_destroy(&ctx->cond);
   mtx_destroy(&ctx->mutex);
}

static void
context_retain(void *data)
{
   struct test_context *ctx = data;
   atomic_fetch_add(&ctx->refs, 1);
}

static void
context_release(void *data)
{
   struct test_context *ctx = data;
   atomic_fetch_sub(&ctx->refs, 1);
}

static VkResult
context_wait(void *data, uint32_t count, const uint64_t *targets,
             uint64_t deadline)
{
   struct test_context *ctx = data;
   mtx_lock(&ctx->mutex);
   ctx->calls++;
   if (ctx->calls == 1)
      ctx->first_deadline = deadline;
   ctx->last_deadline = deadline;
   ctx->expected_count = count;
   CHECK(count <= 64);
   memcpy(ctx->expected_targets, targets, count * sizeof(*targets));
   ctx->entered = true;
   u_cnd_monotonic_broadcast(&ctx->cond);
   uint64_t gate_deadline = TEST_DEADLINE;
   while (ctx->gate && !ctx->proceed) {
      struct timespec until;
      timespec_from_nsec(&until, gate_deadline);
      CHECK(u_cnd_monotonic_timedwait(&ctx->cond, &ctx->mutex, &until) ==
            thrd_success);
   }
   VkResult result = ctx->result;
   struct vk_sync *signal = ctx->signal_on_wait;
   mtx_unlock(&ctx->mutex);
   if (signal)
      CHECK(vk_sync_signal(NULL, signal, 0) == VK_SUCCESS);
   return result;
}

static const struct panvk_kbase_completion_ops completion_ops = {
   .wait = context_wait,
   .retain = context_retain,
   .release = context_release,
};

static struct vk_device test_device;
static struct vk_physical_device test_physical_device;
static const struct vk_sync_type *test_sync_types[] = {
   &panvk_kbase_sync_type,
   NULL,
};

static void
fixture_init(void)
{
   memset(&test_device, 0, sizeof(test_device));
   memset(&test_physical_device, 0, sizeof(test_physical_device));
   test_device.physical = &test_physical_device;
   test_physical_device.supported_sync_types = test_sync_types;
}

static struct vk_sync *
sync_create(uint64_t initial_value)
{
   struct vk_sync *sync = calloc(1, panvk_kbase_sync_type.size);
   CHECK(sync != NULL);
   CHECK(vk_sync_init(NULL, sync, &panvk_kbase_sync_type, 0,
                      initial_value) == VK_SUCCESS);
   return sync;
}

static void
sync_destroy(struct vk_sync *sync)
{
   vk_sync_finish(NULL, sync);
   free(sync);
}

static void
set_pending(struct vk_sync *sync, struct test_context *ctx,
            uint32_t count, const uint64_t *targets)
{
   CHECK(panvk_kbase_sync_set_pending(sync, &completion_ops, ctx,
                                      count, targets) == VK_SUCCESS);
}

static void
wait_context_entered(struct test_context *ctx)
{
   struct timespec until;
   timespec_from_nsec(&until, TEST_DEADLINE);
   mtx_lock(&ctx->mutex);
   while (!ctx->entered) {
      int ret = u_cnd_monotonic_timedwait(&ctx->cond, &ctx->mutex, &until);
      CHECK(ret == thrd_success || ret == thrd_timedout);
      CHECK(ret != thrd_timedout || ctx->entered);
   }
   mtx_unlock(&ctx->mutex);
}

static int
wait_thread(void *data)
{
   struct wait_thread_args *args = data;
   args->result = vk_sync_wait(args->device, args->sync, 0,
                               args->flags, args->deadline);
   return 0;
}

static int
wait_many_thread(void *data)
{
   struct wait_thread_args *args = data;
   struct vk_sync_wait wait = {
      .sync = args->sync,
      .wait_value = 0,
      .stage_mask = ~(VkPipelineStageFlags2)0,
   };
   args->result = vk_sync_wait_many(args->device, 1, &wait,
                                    args->flags, args->deadline);
   return 0;
}

static void
test_binary_state(void)
{
   struct vk_sync *sync = sync_create(0);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   CHECK(vk_sync_signal(NULL, sync, 0) == VK_SUCCESS);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(vk_sync_reset(NULL, sync) == VK_SUCCESS);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   sync_destroy(sync);

   sync = sync_create(1);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   sync_destroy(sync);
}

static void
test_pending_completion_and_copy(void)
{
   struct vk_sync *sync = sync_create(0);
   struct test_context ctx;
   context_init(&ctx, VK_SUCCESS);
   uint64_t targets[] = { 11, 22, 33 };
   set_pending(sync, &ctx, ARRAY_SIZE(targets), targets);
   targets[0] = 99;

   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   CHECK(ctx.calls == 0);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(ctx.calls == 1);
   CHECK(ctx.expected_count == 3);
   CHECK(ctx.expected_targets[0] == 11 && ctx.expected_targets[1] == 22 &&
         ctx.expected_targets[2] == 33);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(ctx.calls == 1);
   sync_destroy(sync);
   context_finish(&ctx);
}

static void
test_timeout_retry_and_error(void)
{
   struct vk_sync *sync = sync_create(0);
   struct test_context ctx;
   context_init(&ctx, VK_TIMEOUT);
   uint64_t target = 4;
   set_pending(sync, &ctx, 1, &target);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   CHECK(ctx.calls == 1);
   mtx_lock(&ctx.mutex);
   ctx.result = VK_SUCCESS;
   ctx.entered = false;
   mtx_unlock(&ctx.mutex);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(ctx.calls == 2);
   sync_destroy(sync);
   context_finish(&ctx);

   sync = sync_create(0);
   context_init(&ctx, VK_ERROR_DEVICE_LOST);
   set_pending(sync, &ctx, 1, &target);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) ==
         VK_ERROR_DEVICE_LOST);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) ==
         VK_ERROR_DEVICE_LOST);
   CHECK(ctx.calls == 1);
   sync_destroy(sync);
   context_finish(&ctx);
}

static void
test_concurrent_resolvers(void)
{
   struct vk_sync *sync = sync_create(0);
   struct test_context ctx;
   context_init(&ctx, VK_SUCCESS);
   ctx.gate = true;
   uint64_t target = 7;
   set_pending(sync, &ctx, 1, &target);

   struct wait_thread_args first = {
      .sync = sync, .flags = VK_SYNC_WAIT_COMPLETE,
      .deadline = TEST_DEADLINE,
   };
   struct wait_thread_args second = {
      .sync = sync, .flags = VK_SYNC_WAIT_COMPLETE,
      .deadline = TEST_DEADLINE,
   };
   thrd_t threads[2];
   CHECK(thrd_create(&threads[0], wait_thread, &first) == thrd_success);
   wait_context_entered(&ctx);
   CHECK(thrd_create(&threads[1], wait_thread, &second) == thrd_success);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);

   mtx_lock(&ctx.mutex);
   ctx.proceed = true;
   u_cnd_monotonic_broadcast(&ctx.cond);
   mtx_unlock(&ctx.mutex);
   CHECK(thrd_join(threads[0], NULL) == thrd_success);
   CHECK(thrd_join(threads[1], NULL) == thrd_success);
   CHECK(first.result == VK_SUCCESS && second.result == VK_SUCCESS);
   CHECK(ctx.calls == 1);
   sync_destroy(sync);
   context_finish(&ctx);
}

static void
test_stale_resolver_after_reset_and_rearm(void)
{
   struct vk_sync *sync = sync_create(0);
   struct test_context old_ctx, new_ctx;
   context_init(&old_ctx, VK_SUCCESS);
   context_init(&new_ctx, VK_SUCCESS);
   old_ctx.gate = true;
   uint64_t target = 1;
   set_pending(sync, &old_ctx, 1, &target);

   struct wait_thread_args args = {
      .sync = sync, .flags = VK_SYNC_WAIT_COMPLETE,
      .deadline = UINT64_MAX,
   };
   thrd_t thread;
   CHECK(thrd_create(&thread, wait_thread, &args) == thrd_success);
   wait_context_entered(&old_ctx);
   CHECK(vk_sync_reset(NULL, sync) == VK_SUCCESS);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   set_pending(sync, &new_ctx, 1, &target);

   mtx_lock(&old_ctx.mutex);
   old_ctx.proceed = true;
   u_cnd_monotonic_broadcast(&old_ctx.cond);
   mtx_unlock(&old_ctx.mutex);
   CHECK(thrd_join(thread, NULL) == thrd_success);
   CHECK(args.result == VK_SUCCESS);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(new_ctx.calls == 1);
   sync_destroy(sync);
   context_finish(&old_ctx);
   context_finish(&new_ctx);
}

static void
test_error_stale_after_move(void)
{
   struct vk_sync *src = sync_create(0);
   struct vk_sync *dst = sync_create(1);
   struct test_context ctx;
   context_init(&ctx, VK_ERROR_DEVICE_LOST);
   ctx.gate = true;
   uint64_t target = 13;
   set_pending(src, &ctx, 1, &target);

   struct wait_thread_args args = {
      .sync = src, .flags = VK_SYNC_WAIT_COMPLETE,
      .deadline = TEST_DEADLINE,
   };
   thrd_t thread;
   CHECK(thrd_create(&thread, wait_thread, &args) == thrd_success);
   wait_context_entered(&ctx);
   CHECK(vk_sync_move(NULL, dst, src) == VK_SUCCESS);
   CHECK(vk_sync_wait(NULL, src, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   CHECK(vk_sync_wait(NULL, dst, 0, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   mtx_lock(&ctx.mutex);
   ctx.proceed = true;
   u_cnd_monotonic_broadcast(&ctx.cond);
   mtx_unlock(&ctx.mutex);
   CHECK(thrd_join(thread, NULL) == thrd_success);
   CHECK(args.result == VK_TIMEOUT);
   CHECK(vk_sync_wait(NULL, dst, 0, VK_SYNC_WAIT_COMPLETE, 0) ==
         VK_ERROR_DEVICE_LOST);
   CHECK(ctx.calls == 1);
   CHECK(vk_sync_move(NULL, dst, dst) == VK_SUCCESS);
   sync_destroy(src);
   sync_destroy(dst);
   context_finish(&ctx);
}

static void
test_reset_releases_detached_callback(void)
{
   struct vk_sync *sync = sync_create(0);
   struct test_context ctx;
   context_init(&ctx, VK_SUCCESS);
   ctx.gate = true;
   uint64_t target = 19;
   set_pending(sync, &ctx, 1, &target);

   struct wait_thread_args args = {
      .sync = sync, .flags = VK_SYNC_WAIT_COMPLETE,
      .deadline = TEST_DEADLINE,
   };
   thrd_t thread;
   CHECK(thrd_create(&thread, wait_thread, &args) == thrd_success);
   wait_context_entered(&ctx);
   CHECK(vk_sync_reset(NULL, sync) == VK_SUCCESS);
   CHECK(atomic_load(&ctx.refs) == 2);
   mtx_lock(&ctx.mutex);
   ctx.proceed = true;
   u_cnd_monotonic_broadcast(&ctx.cond);
   mtx_unlock(&ctx.mutex);
   CHECK(thrd_join(thread, NULL) == thrd_success);
   CHECK(args.result == VK_TIMEOUT);
   CHECK(atomic_load(&ctx.refs) == 1);
   sync_destroy(sync);
   context_finish(&ctx);
}

static void
test_wait_many_any_and_all(void)
{
   struct vk_sync *pending = sync_create(0);
   struct vk_sync *ready = sync_create(1);
   struct test_context ctx;
   context_init(&ctx, VK_TIMEOUT);
   uint64_t target = 5;
   set_pending(pending, &ctx, 1, &target);
   struct vk_sync_wait waits[] = {
      { .sync = pending, .wait_value = 0 },
      { .sync = ready, .wait_value = 0 },
   };

   CHECK(vk_sync_wait_many(&test_device, ARRAY_SIZE(waits), waits,
                           VK_SYNC_WAIT_ANY, TEST_DEADLINE) == VK_SUCCESS);
   CHECK(ctx.calls <= 1);
   CHECK(vk_sync_wait_many(&test_device, ARRAY_SIZE(waits), waits,
                           VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   CHECK(ctx.calls == 2);
   CHECK(vk_sync_wait_many(&test_device, 0, NULL,
                           VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);

   sync_destroy(pending);
   sync_destroy(ready);
   context_finish(&ctx);

   struct vk_sync *first = sync_create(1);
   struct vk_sync *second = sync_create(0);
   struct test_context first_ctx, second_ctx;
   context_init(&first_ctx, VK_SUCCESS);
   context_init(&second_ctx, VK_SUCCESS);
   set_pending(second, &second_ctx, 1, &target);
   waits[0].sync = first;
   waits[1].sync = second;
   CHECK(vk_sync_wait_many(&test_device, ARRAY_SIZE(waits), waits,
                           VK_SYNC_WAIT_COMPLETE, TEST_DEADLINE) == VK_SUCCESS);
   CHECK(second_ctx.calls == 1);
   sync_destroy(first);
   sync_destroy(second);
   context_finish(&first_ctx);
   context_finish(&second_ctx);
}

static void
test_wait_pending_during_resolve_and_host_signal(void)
{
   struct vk_sync *sync = sync_create(0);
   struct test_context ctx;
   context_init(&ctx, VK_SUCCESS);
   ctx.gate = true;
   uint64_t target = 8;
   set_pending(sync, &ctx, 1, &target);

   struct wait_thread_args args = {
      .sync = sync, .flags = VK_SYNC_WAIT_COMPLETE,
      .deadline = TEST_DEADLINE,
   };
   thrd_t thread;
   CHECK(thrd_create(&thread, wait_thread, &args) == thrd_success);
   wait_context_entered(&ctx);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   CHECK(vk_sync_signal(NULL, sync, 0) == VK_SUCCESS);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   mtx_lock(&ctx.mutex);
   ctx.proceed = true;
   u_cnd_monotonic_broadcast(&ctx.cond);
   mtx_unlock(&ctx.mutex);
   CHECK(thrd_join(thread, NULL) == thrd_success);
   CHECK(args.result == VK_SUCCESS);
   CHECK(atomic_load(&ctx.refs) == 1);
   sync_destroy(sync);
   context_finish(&ctx);
}

static void
test_capabilities_and_validation(void)
{
   const struct vk_sync_type *type = &panvk_kbase_sync_type;
   CHECK(type->features == (VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_CPU_WAIT |
                            VK_SYNC_FEATURE_CPU_RESET | VK_SYNC_FEATURE_CPU_SIGNAL |
                            VK_SYNC_FEATURE_WAIT_PENDING | VK_SYNC_FEATURE_WAIT_ANY));
   CHECK(type->wait && type->wait_many && type->move);
   CHECK(!type->import_opaque_fd && !type->export_opaque_fd &&
         !type->import_sync_file && !type->export_sync_file && !type->get_value);
   struct vk_sync *sync = sync_create(0);
   struct test_context ctx;
   context_init(&ctx, VK_SUCCESS);
   uint64_t target = 1;
   CHECK(panvk_kbase_sync_set_pending(sync, NULL, &ctx, 1, &target) != VK_SUCCESS);
   CHECK(panvk_kbase_sync_set_pending(sync, &completion_ops, &ctx, 0, &target) !=
         VK_SUCCESS);
   CHECK(panvk_kbase_sync_set_pending(sync, &completion_ops, &ctx, 65, &target) !=
         VK_SUCCESS);
   CHECK(panvk_kbase_sync_set_pending(sync, &completion_ops, &ctx, 1, NULL) !=
         VK_SUCCESS);
   CHECK(atomic_load(&ctx.refs) == 1);
   sync_destroy(sync);
   context_finish(&ctx);
}

static void
test_nonblocking_stale_signal(void)
{
   struct vk_sync *sync = sync_create(0);
   struct test_context ctx;
   context_init(&ctx, VK_ERROR_DEVICE_LOST);
   ctx.signal_on_wait = sync;
   uint64_t target = 1;
   set_pending(sync, &ctx, 1, &target);
   CHECK(vk_sync_wait(NULL, sync, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(ctx.calls == 1 && ctx.last_deadline == 0);
   CHECK(atomic_load(&ctx.refs) == 1);
   sync_destroy(sync);
   context_finish(&ctx);
}

static void
test_deadlines_and_bounded_wait_any(void)
{
   struct vk_sync *first = sync_create(0);
   struct vk_sync *second = sync_create(0);
   struct test_context a, b;
   context_init(&a, VK_TIMEOUT);
   context_init(&b, VK_TIMEOUT);
   uint64_t target = 42;
   set_pending(first, &a, 1, &target);
   set_pending(second, &b, 1, &target);
   struct vk_sync_wait waits[] = {
      {.sync = first}, {.sync = second},
   };
   uint64_t start = os_time_get_nano();
   uint64_t deadline = start + 20000000ull;
   CHECK(vk_sync_wait_many(&test_device, 2, waits, VK_SYNC_WAIT_ANY, deadline) ==
         VK_TIMEOUT);
   CHECK(a.calls > 1 && a.calls < 200 && b.calls > 1 && b.calls < 200);
   CHECK(a.first_deadline == 0 && a.last_deadline == 0);
   CHECK(b.first_deadline == 0 && b.last_deadline == 0);
   CHECK((uint64_t)os_time_get_nano() >= deadline);

   mtx_lock(&a.mutex);
   a.result = VK_SUCCESS;
   a.calls = 0;
   mtx_unlock(&a.mutex);
   mtx_lock(&b.mutex);
   b.calls = 0;
   mtx_unlock(&b.mutex);
   deadline = os_time_get_nano() + 10000000ull;
   CHECK(vk_sync_wait_many(&test_device, 2, waits, VK_SYNC_WAIT_COMPLETE, deadline) ==
         VK_TIMEOUT);
   CHECK(a.calls == 1 && a.first_deadline <= deadline);
   CHECK(b.calls > 0 && b.last_deadline <= deadline);
   CHECK((uint64_t)os_time_get_nano() >= deadline);
   sync_destroy(first);
   sync_destroy(second);
   context_finish(&a);
   context_finish(&b);
}

static void
test_host_signal_and_pending_move(void)
{
   struct vk_sync *src = sync_create(0);
   struct vk_sync *dst = sync_create(0);
   struct wait_thread_args args = {
      .sync = src, .flags = VK_SYNC_WAIT_COMPLETE, .deadline = UINT64_MAX,
   };
   thrd_t thread;
   CHECK(thrd_create(&thread, wait_thread, &args) == thrd_success);
   CHECK(vk_sync_signal(NULL, src, 0) == VK_SUCCESS);
   CHECK(thrd_join(thread, NULL) == thrd_success);
   CHECK(args.result == VK_SUCCESS);

   struct test_context old_ctx, next_ctx;
   context_init(&old_ctx, VK_SUCCESS);
   context_init(&next_ctx, VK_TIMEOUT);
   uint64_t target = 17;
   set_pending(src, &old_ctx, 1, &target);
   set_pending(dst, &next_ctx, 1, &target);
   CHECK(atomic_load(&old_ctx.refs) == 2 && atomic_load(&next_ctx.refs) == 2);
   CHECK(vk_sync_move(NULL, dst, src) == VK_SUCCESS);
   CHECK(atomic_load(&next_ctx.refs) == 1 && atomic_load(&old_ctx.refs) == 2);
   CHECK(vk_sync_wait(NULL, src, 0, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   CHECK(vk_sync_wait(NULL, dst, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(old_ctx.calls == 1 && next_ctx.calls == 0);
   CHECK(vk_sync_reset(NULL, dst) == VK_SUCCESS);
   CHECK(atomic_load(&old_ctx.refs) == 1);
   sync_destroy(src);
   sync_destroy(dst);
   context_finish(&old_ctx);
   context_finish(&next_ctx);
}

static void
test_prepared_commit_many(void)
{
   struct vk_sync *a = sync_create(0);
   struct vk_sync *b = sync_create(0);
   struct test_context ctx;
   context_init(&ctx, VK_SUCCESS);
   uint64_t target = 7;
   struct panvk_kbase_completion *completion = NULL;
   CHECK(panvk_kbase_completion_prepare(&completion_ops, &ctx, 1, &target,
                                        &completion) == VK_SUCCESS);
   CHECK(completion && atomic_load(&ctx.refs) == 2);
   CHECK(vk_sync_wait(NULL, a, 0, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   CHECK(vk_sync_wait(NULL, b, 0, VK_SYNC_WAIT_PENDING, 0) == VK_TIMEOUT);
   panvk_kbase_completion_unref(completion);
   CHECK(atomic_load(&ctx.refs) == 1);
   CHECK(panvk_kbase_completion_prepare(&completion_ops, &ctx, 1, &target,
                                        &completion) == VK_SUCCESS);
   panvk_kbase_completion_ref(completion);
   struct panvk_kbase_completion *pending[] = {completion, completion};
   struct vk_sync *syncs[] = {a, b};
   panvk_kbase_completion_commit_many(2, syncs, pending);
   CHECK(!pending[0] && !pending[1]);
   CHECK(vk_sync_wait(NULL, a, 0, VK_SYNC_WAIT_PENDING, 0) == VK_SUCCESS);
   CHECK(vk_sync_wait(NULL, b, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(vk_sync_wait(NULL, a, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   CHECK(ctx.calls == 1);
   sync_destroy(a);
   CHECK(atomic_load(&ctx.refs) == 2);
   sync_destroy(b);
   context_finish(&ctx);
}

int
main(void)
{
   fixture_init();
   test_prepared_commit_many();
   test_host_signal_and_pending_move();
   test_capabilities_and_validation();
   test_nonblocking_stale_signal();
   test_deadlines_and_bounded_wait_any();
   test_binary_state();
   test_pending_completion_and_copy();
   test_timeout_retry_and_error();
   test_concurrent_resolvers();
   test_stale_resolver_after_reset_and_rearm();
   test_error_stale_after_move();
   test_reset_releases_detached_callback();
   test_wait_many_any_and_all();
   test_wait_pending_during_resolve_and_host_signal();
   puts("panvk kbase sync tests passed");
   return 0;
}
