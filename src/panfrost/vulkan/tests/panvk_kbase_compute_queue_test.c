#include "drm-uapi/panthor_drm.h"
#include "genxml/cs_builder.h"
#include "genxml/decode.h"
#include "panvk_cmd_buffer.h"
#include "panvk_device_memory.h"
#include "panvk_macros.h"
#include "panvk_queue.h"
#include "kmod/kbase_csf.h"
#include "panvk_kbase_sync.h"
#include "panvk_kbase_csf_encode.h"
#include "kmod/pan_kmod.h"
#include "vk_sync.h"
#include "vk_queue.h"

#include <assert.h>
#include <string.h>

static uint32_t
get_resource_mask(uint32_t subqueue)
{
   return subqueue == PANVK_SUBQUEUE_COMPUTE ? CS_COMPUTE_RES :
          subqueue == PANVK_SUBQUEUE_FRAGMENT ? CS_FRAG_RES : CS_IDVS_RES | CS_TILER_RES;
}

#include "../csf/panvk_kbase_compute_queue.inc"

static unsigned wait_many_calls;
static VkResult wait_many_result = VK_SUCCESS;
static unsigned transport_calls;
static unsigned completion_calls;
static uint64_t last_targets[PANVK_SUBQUEUE_COUNT];
static struct kbase_csf_queue *last_queues[PANVK_SUBQUEUE_COUNT];
static unsigned last_count;
static unsigned heap_create_calls;
static unsigned heap_destroy_calls;
static struct kbase_kmod_heap *initial_heap;
static bool saw_rearm;
static struct kbase_kmod_heap *fail_destroy_heap;
static bool fail_rearm_wait;
static unsigned lost_calls;
static struct vk_sync *expected_signal;
static uint64_t rearm_stream;
static uint64_t recorded_stream_base = 0x300000;
static bool last_was_rearm;
static bool cleanup_active;
static unsigned cleanup_stop_calls;
static unsigned cleanup_queue_unref_calls;
static unsigned cleanup_alias_destroy_calls;
static unsigned cleanup_bo_unref_calls;
static unsigned cleanup_backing_put_calls;
static struct kbase_csf_queue *fail_stop_queue;

void
pan_kmod_bo_put(struct pan_kmod_bo *bo)
{
   assert(cleanup_active && bo && cleanup_stop_calls == PANVK_SUBQUEUE_COUNT);
   assert(cleanup_alias_destroy_calls == 1);
   assert(cleanup_queue_unref_calls == PANVK_SUBQUEUE_COUNT);
   cleanup_backing_put_calls++;
}

int
kbase_csf_queue_stop(struct kbase_csf_queue *queue)
{
   assert(cleanup_active && queue);
   cleanup_stop_calls++;
   return queue == fail_stop_queue ? -1 : 0;
}

void
kbase_csf_queue_unref(struct kbase_csf_queue *queue)
{
   assert(cleanup_active && queue && cleanup_stop_calls == PANVK_SUBQUEUE_COUNT);
   cleanup_queue_unref_calls++;
}

int
kbase_kmod_alias_destroy(struct kbase_kmod_alias *alias)
{
   assert(cleanup_active && alias && cleanup_stop_calls == PANVK_SUBQUEUE_COUNT);
   cleanup_alias_destroy_calls++;
   return 0;
}

void
panvk_priv_bo_unref(struct panvk_priv_bo *bo)
{
   assert(cleanup_active && bo && cleanup_stop_calls == PANVK_SUBQUEUE_COUNT);
   cleanup_bo_unref_calls++;
}

VkResult __wrap__vk_queue_set_lost(struct vk_queue *queue,
                                 const char *file, int line,
                                 const char *msg, ...);

VkResult
__wrap__vk_queue_set_lost(struct vk_queue *queue, const char *file, int line,
                        const char *msg, ...)
{
   lost_calls++;
   return VK_ERROR_DEVICE_LOST;
}

uint64_t panvk_debug;
const struct pan_kmod_ops kbase_kmod_ops = {0};


struct kbase_kmod_heap {
   uint64_t context;
   uint64_t chunk;
};

struct kbase_kmod_heap *
kbase_kmod_heap_create(struct pan_kmod_dev *dev, uint32_t chunk, uint32_t initial,
                       uint32_t max, uint32_t inflight, uint32_t group)
{
   struct kbase_kmod_heap *heap = calloc(1, sizeof(*heap));
   assert(heap && chunk == 1024 * 1024 && initial <= max);
   heap->context = 0x800000 + (++heap_create_calls) * 4096;
   heap->chunk = 0x1000000 + heap_create_calls * 1024 * 1024;
   return heap;
}

int
kbase_kmod_heap_destroy(struct kbase_kmod_heap *heap)
{
   if (cleanup_active)
      assert(cleanup_stop_calls == PANVK_SUBQUEUE_COUNT);
   if (heap == fail_destroy_heap)
      return -1;
   if (heap == initial_heap)
      assert(saw_rearm);
   heap_destroy_calls++;
   free(heap);
   return 0;
}

uint64_t
kbase_kmod_heap_context(struct kbase_kmod_heap *heap)
{
   return heap ? heap->context : 0;
}

uint64_t
kbase_kmod_heap_first_chunk(struct kbase_kmod_heap *heap)
{
   return heap ? heap->chunk : 0;
}

void
pan_kmod_queue_bo_map_sync(struct pan_kmod_bo *bo, uint64_t offset,
                            void *ptr, uint64_t size,
                            enum pan_kmod_bo_sync_type type)
{
}


const struct drm_panthor_csif_info *
panthor_kmod_get_csif_props(const struct pan_kmod_dev *dev)
{
   assert(!"unexpected panthor accessor");
   return NULL;
}

uint32_t
panthor_kmod_get_flush_id(const struct pan_kmod_dev *dev)
{
   assert(!"unexpected panthor flush accessor");
   return 0;
}

uint32_t
kbase_csf_context_flush_id(struct kbase_csf_context *ctx)
{
   return 17;
}

uint64_t
kbase_csf_queue_seqno_va(struct kbase_csf_queue *queue)
{
   return 0x400000;
}

const void *
kbase_csf_queue_seqno_map(struct kbase_csf_queue *queue)
{
   return NULL;
}

uint64_t
kbase_csf_queue_published_target(struct kbase_csf_queue *queue)
{
   return 0;
}

uint64_t
kbase_csf_queue_published_insert(struct kbase_csf_queue *queue)
{
   return 0;
}

uint64_t
kbase_csf_queue_extract(struct kbase_csf_queue *queue)
{
   return 0;
}

uint32_t
kbase_csf_queue_active(struct kbase_csf_queue *queue)
{
   return 0;
}

const void *
kbase_csf_queue_ring_map(struct kbase_csf_queue *queue, uint64_t *size)
{
   return NULL;
}

void
pan_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
}


VkResult __wrap_vk_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                                  const struct vk_sync_wait *waits,
                                  enum vk_sync_wait_flags flags,
                                  uint64_t abs_timeout_ns);

VkResult
__wrap_vk_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                  const struct vk_sync_wait *waits, enum vk_sync_wait_flags flags,
                  uint64_t abs_timeout_ns)
{
   (void)device;
   (void)wait_count;
   (void)waits;
   (void)flags;
   (void)abs_timeout_ns;
   wait_many_calls++;
   return wait_many_result;
}

static bool
check_submit_wrapper(const struct kbase_csf_submit_entry *entry, uint32_t subqueue)
{
   const uint64_t *commands = entry->commands;
   uint32_t regs[128] = {0};
   uint64_t heap = 0, stream = 0;
   uint32_t stream_size = 0;
   unsigned heaps = 0, calls = 0;
   for (unsigned i = 0; i < entry->size / sizeof(*commands); i++) {
      switch (commands[i] >> 56) {
      case MALI_CS_OPCODE_MOVE32: {
         struct MALI_CS_MOVE32 packet;
         MALI_CS_MOVE32_unpack((const struct mali_cs_move32_packed *)&commands[i], &packet);
         assert(packet.destination < ARRAY_SIZE(regs));
         regs[packet.destination] = packet.immediate;
         break;
      }
      case MALI_CS_OPCODE_MOVE48: {
         struct MALI_CS_MOVE48 packet;
         MALI_CS_MOVE48_unpack((const struct mali_cs_move48_packed *)&commands[i], &packet);
         assert(packet.destination + 1 < ARRAY_SIZE(regs));
         regs[packet.destination] = packet.immediate;
         regs[packet.destination + 1] = packet.immediate >> 32;
         break;
      }
      case MALI_CS_OPCODE_HEAP_SET: {
         struct MALI_CS_HEAP_SET packet;
         MALI_CS_HEAP_SET_unpack((const struct mali_cs_heap_set_packed *)&commands[i], &packet);
         assert(packet.address + 1 < ARRAY_SIZE(regs) && !calls);
         heap = regs[packet.address] | ((uint64_t)regs[packet.address + 1] << 32);
         heaps++;
         break;
      }
      case MALI_CS_OPCODE_CALL: {
         struct MALI_CS_CALL packet;
         MALI_CS_CALL_unpack((const struct mali_cs_call_packed *)&commands[i], &packet);
         assert(packet.address + 1 < ARRAY_SIZE(regs) &&
                packet.length < ARRAY_SIZE(regs));
         stream = regs[packet.address] | ((uint64_t)regs[packet.address + 1] << 32);
         stream_size = regs[packet.length];
         calls++;
         break;
      }
      default:
         break;
      }
   }
   assert(calls == 1 && stream_size);
   if (subqueue == PANVK_SUBQUEUE_COMPUTE)
      assert(heaps == 0);
   else
      assert(heaps == 1 && heap == 0x800000 + heap_create_calls * 4096);
   if (rearm_stream && stream == rearm_stream) {
      assert(subqueue != PANVK_SUBQUEUE_COMPUTE && stream_size == sizeof(uint64_t));
      return true;
   }
   assert(stream == recorded_stream_base + subqueue * 4096);
   return false;
}

int
kbase_csf_submit(const struct kbase_csf_submit_entry *entries, uint32_t count)
{
   assert(entries && count > 0 && count <= PANVK_SUBQUEUE_COUNT);
   if (expected_signal)
      assert(vk_sync_wait(NULL, expected_signal, 0, VK_SYNC_WAIT_COMPLETE, 0) ==
             VK_TIMEOUT);
   last_count = count;
   last_was_rearm = false;
   for (unsigned i = 0; i < count; i++) {
      assert(entries[i].commands && entries[i].size > 0);
      uint32_t subqueue = ((uintptr_t)entries[i].queue - 0x2000) / 16;
      assert(subqueue < PANVK_SUBQUEUE_COUNT);
      bool rearm = check_submit_wrapper(&entries[i], subqueue);
      if (i)
         assert(rearm == last_was_rearm);
      last_was_rearm = rearm;
      last_queues[i] = entries[i].queue;
      last_targets[i] = entries[i].target;
   }
   if (last_was_rearm)
      assert(count == 2);
   transport_calls++;
   return 0;
}

int
kbase_csf_wait(struct kbase_csf_queue *const *queues, uint32_t count,
               const uint64_t *targets, uint64_t deadline)
{
   assert(queues && count == last_count && targets);
   for (unsigned i = 0; i < count; i++)
      assert(queues[i] == last_queues[i] && targets[i] == last_targets[i]);
   if (expected_signal)
      assert(vk_sync_wait(NULL, expected_signal, 0, VK_SYNC_WAIT_COMPLETE, 0) ==
             VK_TIMEOUT);
   if (last_was_rearm && fail_rearm_wait)
      return 0;
   if (last_was_rearm)
      saw_rearm = true;
   completion_calls++;
   return 1;
}

int
main(void)
{
   /* This test covers the renewal path itself, so force it on the first
    * graphics submission instead of waiting for the production interval. */
   setenv("PANVK_KBASE_TEST_HEAP_INTERVAL", "1", 1);
   struct panvk_device device = {0};
   struct panvk_gpu_queue queue = {0};
   struct pan_kmod_dev kdev = {.ops = &kbase_kmod_ops};
   device.kmod.dev = &kdev;
   device.kbase.ctx = (void *)0x1000;
   device.kbase.csif_info = (struct drm_panthor_csif_info){
      .cs_reg_count = 128, .unpreserved_cs_reg_count = 4,
      .scoreboard_slot_count = 8,
   };
   device.csf.sb.count = 8;
   device.csf.sb.all_mask = 0xff;
   device.csf.sb.all_iters_mask = 0xf8;
   queue.kbase.queues[PANVK_SUBQUEUE_COMPUTE] = (void *)0x2000;
   struct panvk_priv_bo context_bo = {.addr.dev = 0x100000};
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      queue.kbase.queues[i] = (void *)(uintptr_t)(0x2000 + i * 16);
      queue.subqueues[i].context = (struct panvk_priv_mem){.bo = (uintptr_t)&context_bo};
   }
   queue.vk.base.device = &device.vk;
   queue.kbase.targets[PANVK_SUBQUEUE_COMPUTE] = 17;

   struct vk_queue_submit submit = {0};
   assert(kbase_compute_submit(&queue, &submit) == VK_SUCCESS);
   assert(queue.kbase.targets[PANVK_SUBQUEUE_COMPUTE] == 17);
   assert(wait_many_calls == 1);

   submit.buffer_bind_count = 1;
   assert(kbase_compute_submit(&queue, &submit) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
   assert(wait_many_calls == 1);
   submit.buffer_bind_count = 0;

   struct vk_sync wrong_sync = {0};
   struct vk_sync_wait wait = {
      .sync = &wrong_sync,
      .wait_value = 0,
   };
   submit.wait_count = 1;
   submit.waits = &wait;
   assert(kbase_compute_submit(&queue, &submit) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
   assert(wait_many_calls == 1);

   struct vk_sync kbase_sync = {0};
   kbase_sync.type = &panvk_kbase_sync_type;
   wait.sync = &kbase_sync;
   wait.wait_value = 1;
   assert(kbase_compute_submit(&queue, &submit) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
   assert(wait_many_calls == 1);
   struct vk_sync *consumed = calloc(1, panvk_kbase_sync_type.size);
   assert(consumed && vk_sync_init(NULL, consumed, &panvk_kbase_sync_type, 0, 1) == VK_SUCCESS);
   wait.sync = consumed;
   wait.wait_value = 0;
   wait_many_result = VK_ERROR_DEVICE_LOST;
   assert(kbase_compute_submit(&queue, &submit) == VK_ERROR_DEVICE_LOST);
   assert(vk_sync_wait(NULL, consumed, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   wait_many_result = VK_SUCCESS;
   assert(kbase_compute_submit(&queue, &submit) == VK_SUCCESS);
   assert(vk_sync_wait(NULL, consumed, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_TIMEOUT);
   assert(vk_sync_signal(NULL, consumed, 0) == VK_SUCCESS);
   struct vk_sync_signal reused = {.sync = consumed};
   submit.signal_count = 1;
   submit.signals = &reused;
   assert(kbase_compute_submit(&queue, &submit) == VK_SUCCESS);
   assert(vk_sync_wait(NULL, consumed, 0, VK_SYNC_WAIT_COMPLETE, 0) == VK_SUCCESS);
   vk_sync_finish(NULL, consumed);
   free(consumed);
   submit.signal_count = 0;
   submit.signals = NULL;
   submit.wait_count = 0;
   struct panvk_cmd_buffer cmdbuf = {0};
   uint64_t buffers[PANVK_SUBQUEUE_COUNT][16];
   struct cs_builder_conf conf = {
      .nr_registers = 128, .nr_kernel_registers = 4, .ls_sb_slot = SB_ID(LS),
   };
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct cs_buffer buffer = {.cpu = buffers[i], .gpu = 0x300000 + i * 4096,
                                  .capacity = 16};
      cs_builder_init(&cmdbuf.state.cs[i].builder, &conf, buffer);
      if (i == PANVK_SUBQUEUE_COMPUTE)
         cs_req_res(&cmdbuf.state.cs[i].builder, CS_COMPUTE_RES);
      cs_end(&cmdbuf.state.cs[i].builder);
   }
   struct vk_command_buffer *recorded = &cmdbuf.vk;
   submit.command_buffer_count = 1;
   submit.command_buffers = &recorded;
   assert(kbase_compute_submit(&queue, &submit) == VK_SUCCESS);
   assert(transport_calls == 1 && completion_calls == 1 && queue.kbase.targets[PANVK_SUBQUEUE_COMPUTE] == 18);
   cmdbuf.state.contains_event_waits = true;
   assert(kbase_compute_submit(&queue, &submit) == VK_ERROR_FEATURE_NOT_PRESENT);
   assert(transport_calls == 1);
   cmdbuf.state.contains_event_waits = false;
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      cs_builder_fini(&cmdbuf.state.cs[i].builder);
   struct panvk_cmd_buffer graphics = {0};
   uint64_t gfx_buffers[PANVK_SUBQUEUE_COUNT][16];
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct cs_buffer buffer = {.cpu = gfx_buffers[i], .gpu = 0x500000 + i * 4096,
                                 .capacity = 16};
      cs_builder_init(&graphics.state.cs[i].builder, &conf, buffer);
      cs_req_res(&graphics.state.cs[i].builder, get_resource_mask(i));
      cs_end(&graphics.state.cs[i].builder);
   }
   recorded_stream_base = 0x500000;
   recorded = &graphics.vk;
   assert(kbase_compute_submit(&queue, &submit) == VK_ERROR_FEATURE_NOT_PRESENT);
   assert(transport_calls == 1);
   queue.kbase.graphics_ready = true;
   queue.kbase.heap = kbase_kmod_heap_create(&kdev, 1024 * 1024, 10, 400, 65535, 0);
   initial_heap = queue.kbase.heap;
   uint64_t heap_storage[512] = {0};
   struct pan_kmod_bo heap_kbo = {.dev = &kdev, .size = 4096};
   struct panvk_priv_bo heap_desc = {
      .addr = {.dev = 0x600000, .host = heap_storage}, .bo = &heap_kbo,
   };
   queue.tiler_heap.desc = (struct panvk_priv_mem){.bo = (uintptr_t)&heap_desc};
   uint64_t noop = 0;
   struct panvk_priv_bo init_stream = {.addr = {.dev = 0x700000, .host = &noop}};
   queue.kbase.init_stream = &init_stream;
   rearm_stream = init_stream.addr.dev + PANVK_SUBQUEUE_COUNT * 4096;
   for (unsigned i = 0; i < 2; i++)
      queue.kbase.init_size[i] = sizeof(uint64_t);
   expected_signal = calloc(1, panvk_kbase_sync_type.size);
   assert(expected_signal);
   assert(vk_sync_init(NULL, expected_signal, &panvk_kbase_sync_type, 0, 0) ==
          VK_SUCCESS);
   struct vk_sync_signal signal = {.sync = expected_signal};
   submit.signal_count = 1;
   submit.signals = &signal;
   /* The first-frame report must describe the fixture as it actually is, and
    * must not invent readiness.  It is read-only scaffolding: calling it does
    * not submit, encode or allocate.
    */
   {
      unsigned transport_before = transport_calls;
      struct kbase_first_frame_report report =
         kbase_first_frame_probe(&queue, &submit);
      assert(transport_calls == transport_before);
      assert(report.graphics_ready == queue.kbase.graphics_ready);
      assert(report.has_tiler_heap == (queue.kbase.heap != NULL));
      assert(report.desc_alias_present == (queue.kbase.desc_alias != NULL));
      assert(report.tiler_heap_context == queue.tiler_heap.context.dev_addr);
      assert(report.has_graphics_chunk);
      assert(report.graphics_subqueues ==
             ((1u << 0) | (1u << 1)));
   }
   assert(kbase_compute_submit(&queue, &submit) == VK_SUCCESS);
   assert(vk_sync_wait(NULL, expected_signal, 0, VK_SYNC_WAIT_COMPLETE, 0) ==
          VK_SUCCESS);
   assert(saw_rearm && heap_create_calls == 2 && heap_destroy_calls == 1);
   assert(queue.kbase.retired_heap == NULL);
   assert(transport_calls == 3 && completion_calls == 3 && last_count == 3);
   assert(queue.kbase.targets[PANVK_SUBQUEUE_COMPUTE] == 19);
   assert(queue.kbase.targets[0] == 2 && queue.kbase.targets[1] == 2);
   struct kbase_kmod_heap *old_heap = queue.kbase.heap;
   unsigned destroyed_before_failure = heap_destroy_calls;
   assert(vk_sync_reset(NULL, expected_signal) == VK_SUCCESS);
   saw_rearm = false;
   fail_rearm_wait = true;
   assert(kbase_compute_submit(&queue, &submit) == VK_ERROR_DEVICE_LOST);
   assert(lost_calls == 1 && queue.kbase.retired_heap == old_heap);
   assert(queue.kbase.heap != old_heap);
   assert(heap_destroy_calls == destroyed_before_failure && !saw_rearm);
   assert(transport_calls == 4 && completion_calls == 3);
   assert(queue.kbase.targets[0] == 3 && queue.kbase.targets[1] == 3);
   assert(vk_sync_wait(NULL, expected_signal, 0, VK_SYNC_WAIT_COMPLETE, 0) ==
          VK_TIMEOUT);
   fail_rearm_wait = false;

   struct panvk_gpu_queue retire_queue = queue;
   retire_queue.kbase.retired_heap = NULL;
   retire_queue.kbase.heap = kbase_kmod_heap_create(&kdev, 1024 * 1024, 10,
                                                  400, 65535, 0);
   fail_destroy_heap = retire_queue.kbase.heap;
   unsigned transports_before_retirement = transport_calls;
   unsigned completions_before_retirement = completion_calls;
   assert(kbase_compute_submit(&retire_queue, &submit) == VK_ERROR_DEVICE_LOST);
   assert(lost_calls == 2 && device.kbase.quarantined);
   assert(retire_queue.kbase.retired_heap == fail_destroy_heap);
   assert(retire_queue.kbase.heap != fail_destroy_heap && saw_rearm);
   assert(heap_destroy_calls == destroyed_before_failure);
   assert(transport_calls == transports_before_retirement + 1);
   assert(completion_calls == completions_before_retirement + 1);
   assert(vk_sync_wait(NULL, expected_signal, 0, VK_SYNC_WAIT_COMPLETE, 0) ==
          VK_TIMEOUT);
   fail_destroy_heap = NULL;
   assert(kbase_kmod_heap_destroy(retire_queue.kbase.retired_heap) == 0);
   assert(kbase_kmod_heap_destroy(retire_queue.kbase.heap) == 0);

   vk_sync_finish(NULL, expected_signal);
   free(expected_signal);
   expected_signal = NULL;
   queue.kbase.desc_alias = (void *)0x9000;
   struct pan_kmod_bo ring_bo = {.dev = &kdev};
   queue.render_desc_ringbuf.bo = &ring_bo;
   cleanup_active = true;
   fail_stop_queue = queue.kbase.queues[PANVK_SUBQUEUE_VERTEX_TILER];
   unsigned destroyed_before_stop = heap_destroy_calls;
   assert(!kbase_compute_cleanup(&queue));
   assert(device.kbase.quarantined);
   assert(cleanup_stop_calls == PANVK_SUBQUEUE_COUNT);
   assert(cleanup_queue_unref_calls == 0 && cleanup_alias_destroy_calls == 0);
   assert(cleanup_bo_unref_calls == 0 && cleanup_backing_put_calls == 0);
   assert(heap_destroy_calls == destroyed_before_stop);
   assert(queue.kbase.heap && queue.kbase.retired_heap && queue.kbase.desc_alias);
   assert(queue.kbase.init_stream == &init_stream);
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      assert(queue.kbase.queues[i] && queue.subqueues[i].context.bo);
   cleanup_stop_calls = 0;
   fail_stop_queue = NULL;
   initial_heap = NULL;
   assert(kbase_compute_cleanup(&queue));
   assert(cleanup_stop_calls == PANVK_SUBQUEUE_COUNT);
   assert(cleanup_queue_unref_calls == PANVK_SUBQUEUE_COUNT);
   assert(cleanup_alias_destroy_calls == 1 && cleanup_backing_put_calls == 1);
   assert(cleanup_bo_unref_calls == PANVK_SUBQUEUE_COUNT + 2);
   assert(heap_destroy_calls == destroyed_before_stop + 2);
   assert(!queue.kbase.heap && !queue.kbase.retired_heap && !queue.kbase.desc_alias);
   assert(!queue.kbase.init_stream && !queue.tiler_heap.desc.bo);
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      assert(!queue.kbase.queues[i] && !queue.subqueues[i].context.bo);
      cs_builder_fini(&graphics.state.cs[i].builder);
   }

   return 0;
}

/* KCPU probe stubs.  The probe is off unless PANVK_DEBUG=kcpu_event, but the
 * submit path still references these, so the test needs definitions. */
struct kbase_csf_kcpu { int unused; };
struct kbase_csf_kcpu *
kbase_csf_kcpu_create(struct kbase_csf_context *ctx)
{
   return NULL;
}

void
kbase_csf_kcpu_destroy(struct kbase_csf_kcpu *kcpu)
{
}

int
kbase_csf_kcpu_cqs_wait(struct kbase_csf_kcpu *kcpu, uint64_t addr, uint32_t val)
{
   return -1;
}

int
kbase_csf_kcpu_fence_signal(struct kbase_csf_kcpu *kcpu, int *fd)
{
   return -1;
}
