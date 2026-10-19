#include "drm-uapi/panthor_drm.h"
#include "genxml/cs_builder.h"
#include "panvk_cmd_buffer.h"
#include "panvk_device_memory.h"
#include "panvk_macros.h"
#include "panvk_queue.h"
#include "kmod/kbase_csf.h"
#include "panvk_kbase_sync.h"
#include "panvk_kbase_csf_encode.h"
#include "vk_queue.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint32_t
get_resource_mask(uint32_t subqueue)
{
   return subqueue == PANVK_SUBQUEUE_COMPUTE ? CS_COMPUTE_RES :
          subqueue == PANVK_SUBQUEUE_FRAGMENT ? CS_FRAG_RES :
          CS_IDVS_RES | CS_TILER_RES;
}

#include "../csf/panvk_kbase_compute_queue.inc"

struct kbase_csf_queue {
   unsigned id;
   bool live;
};

struct kbase_kmod_heap {
   uint64_t context;
   uint64_t chunk;
   bool live;
};

struct kbase_kmod_alias {
   struct pan_kmod_bo *backing;
   uint64_t address;
   bool live;
};

struct allocation {
   struct panvk_priv_bo priv;
   struct pan_kmod_bo kbo;
   void *host;
   size_t size;
   unsigned flushes;
   bool native;
   bool live;
};

static struct {
   struct panvk_device device;
   struct panvk_gpu_queue queue;
   struct pan_kmod_dev kdev;
   struct pan_kmod_vm vm;
   uint32_t dump_sizes[PANVK_SUBQUEUE_COUNT];
   struct allocation allocs[16];
   unsigned allocation_count;
   struct kbase_csf_queue queues[PANVK_SUBQUEUE_COUNT];
   struct kbase_kmod_heap heaps[2];
   unsigned heap_count;
   struct kbase_kmod_alias alias;
   unsigned acquired;
   unsigned fail_acquire;
   unsigned jit_calls;
   unsigned fail_jit;
   unsigned queue_count;
   unsigned stopped_mask;
   unsigned fail_stop;
   struct kbase_kmod_heap *fail_heap;
   bool fail_alias;
   bool host_oom;
   bool cleanup;
   bool submit_failure;
   int wait_result;
   int bad_seqno;
   unsigned pool_count;
   unsigned flush_calls;
   unsigned init_flush_mask;
   unsigned submit_calls;
   unsigned wait_calls;
   unsigned lost_calls;
   unsigned releases;
} fixture;

uint64_t panvk_debug;
const struct pan_kmod_ops kbase_kmod_ops = {0};


VkResult __wrap__vk_queue_set_lost(struct vk_queue *queue, const char *file,
                                 int line, const char *message, ...);

VkResult
__wrap__vk_queue_set_lost(struct vk_queue *queue, const char *file,
                        int line, const char *message, ...)
{
   assert(queue == &fixture.queue.vk);
   fixture.lost_calls++;
   return VK_ERROR_DEVICE_LOST;
}

static unsigned
live_queue_mask(void)
{
   unsigned mask = 0;
   for (unsigned i = 0; i < fixture.queue_count; i++)
      if (fixture.queues[i].live)
         mask |= 1u << i;
   return mask;
}

static void
check_stopped(void)
{
   assert(fixture.cleanup);
   assert((fixture.stopped_mask & live_queue_mask()) == live_queue_mask());
   assert(fixture.fail_stop == UINT32_MAX);
}

static void
before_release(void)
{
   check_stopped();
   fixture.releases++;
}

static bool
acquire(void)
{
   assert(!fixture.cleanup && fixture.jit_calls);
   fixture.acquired++;
   if (fixture.acquired == fixture.fail_acquire) {
      errno = fixture.host_oom ? -ENOMEM : ENOMEM;
      return false;
   }
   return true;
}

static struct allocation *
allocation_create(size_t size, uint32_t flags, bool native)
{
   assert(fixture.allocation_count < ARRAY_SIZE(fixture.allocs));
   struct allocation *alloc = &fixture.allocs[fixture.allocation_count++];
   alloc->host = aligned_alloc(4096, ALIGN_POT(size + 64, 4096));
   assert(alloc->host);
   memset(alloc->host, 0xa5, size + 64);
   alloc->size = size;
   alloc->native = native;
   alloc->live = true;
   alloc->kbo = (struct pan_kmod_bo){
      .dev = &fixture.kdev, .size = size, .flags = flags, .refcnt = 1,
   };
   alloc->priv = (struct panvk_priv_bo){
      .bo = &alloc->kbo, .dev = &fixture.device, .refcnt = 1,
      .addr = {.host = alloc->host,
               .dev = 0x1000000ull + fixture.allocation_count * 0x100000},
   };
   return alloc;
}

static struct allocation *
find_bo(struct pan_kmod_bo *bo)
{
   for (unsigned i = 0; i < fixture.allocation_count; i++)
      if (&fixture.allocs[i].kbo == bo) {
         assert(fixture.allocs[i].live);
         return &fixture.allocs[i];
      }
   abort();
}

static void
check_canary(const struct allocation *alloc)
{
   assert(alloc->host && alloc->live);
   const uint8_t *bytes = alloc->host;
   for (unsigned i = 0; i < 64; i++)
      assert(bytes[alloc->size + i] == 0xa5);
}

static void
check_all_live(void)
{
   assert(live_queue_mask() == BITFIELD_MASK(PANVK_SUBQUEUE_COUNT));
   assert(fixture.heaps[0].live && fixture.alias.live);
   for (unsigned i = 0; i < fixture.allocation_count; i++)
      check_canary(&fixture.allocs[i]);
}

int
kbase_kmod_jit_init(struct pan_kmod_dev *dev, uint64_t va_pages,
                    uint64_t phys_pages, uint32_t max_allocations)
{
   assert(dev == &fixture.kdev && va_pages == (1ull << 25));
   assert(phys_pages == va_pages && max_allocations == 255);
   fixture.jit_calls++;
   if (fixture.jit_calls == 1)
      assert(fixture.acquired == 0);
   else
      assert(fixture.jit_calls == 2 && fixture.acquired == PANVK_SUBQUEUE_COUNT);
   return fixture.jit_calls == fixture.fail_jit ? -1 : 0;
}

struct kbase_csf_queue *
kbase_csf_queue_create(struct kbase_csf_context *ctx, uint32_t ring_size,
                       uint8_t priority)
{
   assert(ctx == fixture.device.kbase.ctx && ring_size == 65536 && priority == 1);
   assert(fixture.jit_calls == 1);
   if (!acquire())
      return NULL;
   unsigned id = fixture.queue_count++;
   assert(id < PANVK_SUBQUEUE_COUNT);
   fixture.queues[id] = (struct kbase_csf_queue){.id = id, .live = true};
   return &fixture.queues[id];
}

int
kbase_csf_queue_stop(struct kbase_csf_queue *queue)
{
   assert(fixture.cleanup && queue->live);
   assert(!(fixture.stopped_mask & (1u << queue->id)));
   fixture.stopped_mask |= 1u << queue->id;
   return queue->id == fixture.fail_stop ? -1 : 0;
}

void
kbase_csf_queue_unref(struct kbase_csf_queue *queue)
{
   before_release();
   assert(queue->live);
   queue->live = false;
}

uint64_t
kbase_csf_queue_seqno_va(struct kbase_csf_queue *queue)
{
   assert(queue->live);
   return 0x400000 + queue->id * 4096 + (queue->id == fixture.bad_seqno ? 1 : 0);
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

uint32_t
kbase_csf_context_flush_id(struct kbase_csf_context *ctx)
{
   assert(ctx == fixture.device.kbase.ctx);
   return 17;
}

const struct drm_panthor_csif_info *
panthor_kmod_get_csif_props(const struct pan_kmod_dev *dev)
{
   abort();
}

uint32_t
panthor_kmod_get_flush_id(const struct pan_kmod_dev *dev)
{
   abort();
}

struct kbase_kmod_heap *
kbase_kmod_heap_create(struct pan_kmod_dev *dev, uint32_t chunk_size,
                       uint32_t initial_chunks, uint32_t max_chunks,
                       uint32_t target_in_flight, uint32_t group)
{
   assert(dev == &fixture.kdev && fixture.jit_calls == 2);
   assert(chunk_size == 1024 * 1024 && initial_chunks == 10 && max_chunks == 400);
   assert(target_in_flight == UINT16_MAX && group == 0);
   if (!acquire())
      return NULL;
   assert(fixture.heap_count < ARRAY_SIZE(fixture.heaps));
   struct kbase_kmod_heap *heap = &fixture.heaps[fixture.heap_count++];
   *heap = (struct kbase_kmod_heap){
      .context = 0x800000 + fixture.heap_count * 4096,
      .chunk = 0x90000000 + fixture.heap_count * 1024 * 1024, .live = true,
   };
   return heap;
}

int
kbase_kmod_heap_destroy(struct kbase_kmod_heap *heap)
{
   assert(heap->live);
   check_stopped();
   if (heap == fixture.fail_heap) {
      assert(fixture.cleanup);
      return -1;
   }
   before_release();
   heap->live = false;
   return 0;
}

uint64_t
kbase_kmod_heap_context(struct kbase_kmod_heap *heap)
{
   assert(heap && heap->live);
   return heap->context;
}

uint64_t
kbase_kmod_heap_first_chunk(struct kbase_kmod_heap *heap)
{
   assert(heap && heap->live);
   return heap->chunk;
}

struct panvk_priv_mem
panvk_pool_alloc_mem(struct panvk_pool *pool, struct panvk_pool_alloc_info info)
{
   assert(pool == &fixture.device.mempools.rw);
   fixture.pool_count++;
   if (fixture.pool_count == 1)
      assert(info.size == 4096 + 65536 && info.alignment == 4096);
   else {
      assert(info.alignment == 64);
      if (fixture.pool_count == 2)
         assert(info.size == sizeof(struct panvk_cs_sync32));
      else if (fixture.pool_count == 3)
         assert(info.size == ALIGN_POT(sizeof(struct panvk_cs_sync64), 64) *
                             PANVK_SUBQUEUE_COUNT);
      else {
         bool save_region = false;
         for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
            save_region |= fixture.dump_sizes[i] == info.size;
         assert(info.size == sizeof(struct panvk_cs_subqueue_context) || save_region);
      }
   }
   if (!acquire())
      return (struct panvk_priv_mem){0};
   struct allocation *alloc = allocation_create(info.size, PAN_KMOD_BO_FLAG_WB_MMAP, false);
   assert(info.alignment && !(info.alignment & (info.alignment - 1)));
   assert(!((uintptr_t)alloc->host & (info.alignment - 1)));
   assert(!(alloc->priv.addr.dev & (info.alignment - 1)));
   return (struct panvk_priv_mem){.bo = (uintptr_t)&alloc->priv};
}

VkResult
panvk_priv_bo_create(struct panvk_device *dev, uint64_t size, uint32_t flags,
                     VkSystemAllocationScope scope, struct panvk_priv_bo **out)
{
   assert(dev == &fixture.device && size == 4096 * (PANVK_SUBQUEUE_COUNT + 1));
   assert(flags == PAN_KMOD_BO_FLAG_WB_MMAP && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!acquire()) {
      *out = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   *out = &allocation_create(size, flags, false)->priv;
   return VK_SUCCESS;
}

void
panvk_priv_bo_unref(struct panvk_priv_bo *bo)
{
   struct allocation *alloc = find_bo(bo->bo);
   before_release();
   assert(!alloc->native && &alloc->priv == bo);
   check_canary(alloc);
   alloc->live = false;
   free(alloc->host);
   alloc->host = NULL;
}

struct pan_kmod_bo *
pan_kmod_bo_alloc(struct pan_kmod_dev *dev, struct pan_kmod_vm *vm,
                  uint64_t size, uint32_t flags)
{
   assert(dev == &fixture.kdev && vm == &fixture.vm);
   assert(size == RENDER_DESC_RINGBUF_SIZE && flags == 0);
   if (!acquire())
      return NULL;
   return &allocation_create(size, flags, true)->kbo;
}

void
pan_kmod_bo_put(struct pan_kmod_bo *bo)
{
   struct allocation *alloc = find_bo(bo);
   before_release();
   assert(alloc->native && !fixture.alias.live && bo->refcnt == 1);
   check_canary(alloc);
   bo->refcnt = 0;
   alloc->live = false;
   free(alloc->host);
   alloc->host = NULL;
}

struct kbase_kmod_alias *
kbase_kmod_alias_create(struct pan_kmod_bo *bo, uint64_t size, uint32_t count)
{
   struct allocation *alloc = find_bo(bo);
   assert(alloc->native && size == RENDER_DESC_RINGBUF_SIZE && count == 2);
   if (!acquire())
      return NULL;
   assert(!fixture.alias.live && bo->refcnt == 1);
   bo->refcnt++;
   fixture.alias = (struct kbase_kmod_alias){
      .backing = bo, .address = 0xb0000000, .live = true,
   };
   return &fixture.alias;
}

int
kbase_kmod_alias_destroy(struct kbase_kmod_alias *alias)
{
   assert(alias == &fixture.alias && alias->live);
   check_stopped();
   if (fixture.fail_alias) {
      assert(fixture.cleanup);
      return -1;
   }
   before_release();
   assert(alias->backing->refcnt == 2);
   alias->backing->refcnt--;
   alias->live = false;
   return 0;
}

uint64_t
kbase_kmod_alias_gpu_va(struct kbase_kmod_alias *alias)
{
   assert(alias == &fixture.alias && alias->live);
   return alias->address;
}

uint64_t
kbase_kmod_bo_gpu_va(struct pan_kmod_bo *bo)
{
   struct allocation *alloc = find_bo(bo);
   return (uintptr_t)alloc->host;
}

void
pan_kmod_queue_bo_map_sync(struct pan_kmod_bo *bo, uint64_t offset,
                           void *ptr, uint64_t size, enum pan_kmod_bo_sync_type type)
{
   struct allocation *alloc = find_bo(bo);
   assert(offset <= alloc->size && size <= alloc->size - offset);
   assert(ptr == (uint8_t *)alloc->host + offset);
   assert(type == PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH);
   alloc->flushes++;
}

void
panvk_priv_bo_flush(struct panvk_priv_bo *bo, size_t offset, size_t size)
{
   assert(bo == fixture.queue.kbase.init_stream && offset % 4096 == 0);
   unsigned slice = offset / 4096;
   assert(slice <= PANVK_SUBQUEUE_COUNT && !(fixture.init_flush_mask & (1u << slice)));
   assert(size == (slice == PANVK_SUBQUEUE_COUNT ? sizeof(uint64_t) :
                   fixture.queue.kbase.init_size[slice]));
   fixture.init_flush_mask |= 1u << slice;
   pan_kmod_queue_bo_map_sync(bo->bo, offset, (uint8_t *)bo->addr.host + offset,
                             size, PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH);
}

void
pan_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
   assert(dev == &fixture.kdev && !fixture.cleanup);
   fixture.flush_calls++;
}

static uint64_t
reg64(const uint32_t *regs, unsigned index)
{
   assert(index + 1 < 128);
   return regs[index] | ((uint64_t)regs[index + 1] << 32);
}

static void
check_init_stream(unsigned id)
{
   const uint64_t *commands = (const void *)((const uint8_t *)fixture.queue.kbase.init_stream->addr.host + id * 4096);
   unsigned words = fixture.queue.kbase.init_size[id] / sizeof(*commands);
   uint32_t regs[128];
   memset(regs, 0xa5, sizeof(regs));
   bool written[128] = {false};
   for (unsigned i = 0; i < words; i++) {
      if (commands[i] >> 56 == MALI_CS_OPCODE_MOVE48) {
         struct MALI_CS_MOVE48 packet;
         MALI_CS_MOVE48_unpack((const struct mali_cs_move48_packed *)&commands[i], &packet);
         assert(packet.destination + 1 < ARRAY_SIZE(regs));
         regs[packet.destination] = packet.immediate;
         regs[packet.destination + 1] = packet.immediate >> 32;
         written[packet.destination] = written[packet.destination + 1] = true;
      } else {
         assert(commands[i] >> 56 == MALI_CS_OPCODE_MOVE32);
         struct MALI_CS_MOVE32 packet;
         MALI_CS_MOVE32_unpack((const struct mali_cs_move32_packed *)&commands[i], &packet);
         assert(packet.destination < ARRAY_SIZE(regs));
         regs[packet.destination] = packet.immediate;
         written[packet.destination] = true;
      }
   }
   assert(reg64(regs, PANVK_CS_REG_SUBQUEUE_CTX_START) ==
          panvk_priv_mem_dev_addr(fixture.queue.subqueues[id].context));
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      unsigned reg = PANVK_CS_REG_PROGRESS_SEQNO_START + i * 2;
      assert(written[reg] && written[reg + 1] && reg64(regs, reg) == 0);
   }
   assert(words > 0 && words * sizeof(*commands) < 4096);
}

static void
check_wrapper(const struct kbase_csf_submit_entry *entry, unsigned id)
{
   assert(entry->queue == &fixture.queues[id] && entry->target == 1);
   const uint64_t *commands = entry->commands;
   uint32_t regs[128] = {0};
   unsigned heaps = 0, calls = 0, resources = 0, syncs = 0;
   assert(entry->size && !(entry->size & 7));
   for (unsigned i = 0; i < entry->size / sizeof(*commands); i++) {
      switch (commands[i] >> 56) {
      case MALI_CS_OPCODE_MOVE48: {
         struct MALI_CS_MOVE48 packet;
         MALI_CS_MOVE48_unpack((const struct mali_cs_move48_packed *)&commands[i], &packet);
         assert(packet.destination + 1 < ARRAY_SIZE(regs));
         regs[packet.destination] = packet.immediate;
         regs[packet.destination + 1] = packet.immediate >> 32;
         break;
      }
      case MALI_CS_OPCODE_MOVE32: {
         struct MALI_CS_MOVE32 packet;
         MALI_CS_MOVE32_unpack((const struct mali_cs_move32_packed *)&commands[i], &packet);
         assert(packet.destination < ARRAY_SIZE(regs));
         regs[packet.destination] = packet.immediate;
         break;
      }
      case MALI_CS_OPCODE_HEAP_SET: {
         struct MALI_CS_HEAP_SET packet;
         MALI_CS_HEAP_SET_unpack((const struct mali_cs_heap_set_packed *)&commands[i], &packet);
         assert(!calls && reg64(regs, packet.address) == fixture.heaps[0].context);
         heaps++;
         break;
      }
      case MALI_CS_OPCODE_CALL: {
         struct MALI_CS_CALL packet;
         MALI_CS_CALL_unpack((const struct mali_cs_call_packed *)&commands[i], &packet);
         assert(packet.length < ARRAY_SIZE(regs));
         assert(reg64(regs, packet.address) == fixture.queue.kbase.init_stream->addr.dev + id * 4096);
         assert(regs[packet.length] == fixture.queue.kbase.init_size[id]);
         assert(reg64(regs, PANVK_CS_REG_SUBQUEUE_CTX_START) ==
                panvk_priv_mem_dev_addr(fixture.queue.subqueues[id].context));
         calls++;
         break;
      }
      case MALI_CS_OPCODE_REQ_RESOURCE: {
         struct MALI_CS_REQ_RESOURCE packet;
         MALI_CS_REQ_RESOURCE_unpack((const struct mali_cs_req_resource_packed *)&commands[i], &packet);
         assert(packet.compute == (id == PANVK_SUBQUEUE_COMPUTE));
         assert(packet.fragment == (id == PANVK_SUBQUEUE_FRAGMENT));
         assert(packet.tiler == (id == PANVK_SUBQUEUE_VERTEX_TILER));
         assert(packet.idvs == (id == PANVK_SUBQUEUE_VERTEX_TILER) && !packet.rt);
         resources++;
         break;
      }
      case MALI_CS_OPCODE_SYNC_ADD64: {
         struct MALI_CS_SYNC_ADD64 packet;
         MALI_CS_SYNC_ADD64_unpack((const struct mali_cs_sync_add64_packed *)&commands[i], &packet);
         assert(calls == 1 && packet.error_propagate && packet.scope == MALI_CS_SYNC_SCOPE_SYSTEM);
         assert(reg64(regs, packet.address) == kbase_csf_queue_seqno_va(entry->queue));
         assert(reg64(regs, packet.data) == 1);
         syncs++;
         break;
      }
      default:
         break;
      }
   }
   assert(calls == 1 && heaps == (id != PANVK_SUBQUEUE_COMPUTE));
   assert(resources == 1 && syncs == 1);
}

static void
check_initialization(void)
{
   unsigned save_regions = 0;
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      save_regions += fixture.dump_sizes[i] != 0;
   assert(fixture.jit_calls == 2 && fixture.acquired == 13 + save_regions);
   assert(fixture.init_flush_mask == BITFIELD_MASK(PANVK_SUBQUEUE_COUNT + 1));
   assert(fixture.queue.kbase.graphics_ready && fixture.queue.kbase.heap == &fixture.heaps[0]);
   assert(fixture.queue.render_desc_ringbuf.bo == fixture.alias.backing);
   assert(fixture.alias.backing->refcnt == 2);
   assert(fixture.queue.render_desc_ringbuf.size == RENDER_DESC_RINGBUF_SIZE);
   assert(fixture.queue.render_desc_ringbuf.addr.dev == fixture.alias.address);
   struct panvk_priv_mem desc = fixture.queue.tiler_heap.desc;
   struct MALI_TILER_HEAP heap;
   MALI_TILER_HEAP_unpack((const struct mali_tiler_heap_packed *)panvk_priv_mem_host_addr(desc), &heap);
   assert(heap.size == 1024 * 1024 && heap.base == fixture.heaps[0].chunk);
   assert(heap.bottom == heap.base + 64 && heap.top == heap.base + heap.size);
   assert(panvk_priv_mem_dev_addr(desc) != fixture.heaps[0].context);
   struct panvk_cs_sync32 *ring = panvk_priv_mem_host_addr(fixture.queue.render_desc_ringbuf.syncobj);
   assert(ring->seqno == RENDER_DESC_RINGBUF_SIZE && ring->error == 0);
   struct panvk_cs_sync64 *syncs = panvk_priv_mem_host_addr(fixture.queue.syncobjs);
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      assert(syncs[i].seqno == 1 && syncs[i].error == 0 && syncs[i].pad == 0);
      const struct panvk_cs_subqueue_context *ctx = panvk_priv_mem_host_addr(fixture.queue.subqueues[i].context);
      struct panvk_cs_subqueue_context expected = {
         .syncobjs = panvk_priv_mem_dev_addr(fixture.queue.syncobjs),
      };
      if (i != PANVK_SUBQUEUE_COMPUTE) {
         expected.render.tiler_heap = panvk_priv_mem_dev_addr(desc);
         expected.render.geom_buf = (expected.render.tiler_heap + 4096) | 16;
         expected.render.desc_ringbuf.syncobj = panvk_priv_mem_dev_addr(fixture.queue.render_desc_ringbuf.syncobj);
         expected.render.desc_ringbuf.ptr = fixture.alias.address;
      }
      assert(ctx->syncobjs == expected.syncobjs && !ctx->pad && !ctx->last_error);
      uint64_t dump_address = panvk_priv_mem_dev_addr(fixture.queue.subqueues[i].regs_save);
      assert(ctx->reg_dump_addr == dump_address);
      assert(!!dump_address == !!fixture.dump_sizes[i]);
      assert(ctx->render.tiler_heap == expected.render.tiler_heap);
      assert(ctx->render.geom_buf == expected.render.geom_buf);
      assert(ctx->render.desc_ringbuf.syncobj == expected.render.desc_ringbuf.syncobj);
      assert(ctx->render.desc_ringbuf.ptr == expected.render.desc_ringbuf.ptr);
      assert(!ctx->render.desc_ringbuf.pos && !ctx->render.desc_ringbuf.pad);
      assert(!ctx->render.oq_chain.head && !ctx->render.oq_chain.tail);
      assert(!ctx->render.ts_chain.head && !ctx->render.ts_chain.tail);
      assert(!ctx->render.ts_done_chain.head && !ctx->render.ts_done_chain.tail);
      assert(!ctx->render.td_count && !ctx->render.last_td_fullscreen_tiler_flags);
      assert(!ctx->tiler_oom_ctx.counter && !ctx->tiler_oom_ctx.layer_fbd_ptr);
      assert(!ctx->tiler_oom_ctx.ir_scratch_fbd_ptr && !ctx->tiler_oom_ctx.td_count);
      assert(!ctx->tiler_oom_ctx.layer_count && !ctx->tiler_oom_ctx.crc_header_addr_count);
      for (unsigned j = 0; j < ARRAY_SIZE(ctx->tiler_oom_ctx.ir_descs); j++)
         assert(!ctx->tiler_oom_ctx.ir_descs[j]);
      for (unsigned j = 0; j < ARRAY_SIZE(ctx->tiler_oom_ctx.crc_header_addrs); j++)
         assert(!ctx->tiler_oom_ctx.crc_header_addrs[j]);
      assert(!ctx->debug.tracebuf.cs && !ctx->cond_render_flag);
      check_init_stream(i);
   }
   const uint64_t *noop = (const void *)((const uint8_t *)fixture.queue.kbase.init_stream->addr.host + PANVK_SUBQUEUE_COUNT * 4096);
   assert(*noop == 0);
   for (unsigned i = 0; i < fixture.allocation_count; i++) {
      if (!fixture.allocs[i].native &&
          &fixture.allocs[i].priv != panvk_priv_mem_bo(fixture.queue.subqueues[0].regs_save) &&
          &fixture.allocs[i].priv != panvk_priv_mem_bo(fixture.queue.subqueues[1].regs_save) &&
          &fixture.allocs[i].priv != panvk_priv_mem_bo(fixture.queue.subqueues[2].regs_save))
         assert(fixture.allocs[i].flushes >= 1);
   }
   struct allocation *init = find_bo(fixture.queue.kbase.init_stream->bo);
   assert(init->flushes == PANVK_SUBQUEUE_COUNT + 1);
}

int
kbase_csf_submit(const struct kbase_csf_submit_entry *entries, uint32_t count)
{
   assert(count == PANVK_SUBQUEUE_COUNT && fixture.flush_calls == 1);
   assert(fixture.wait_calls == 0 && fixture.submit_calls == 0);
   check_all_live();
   check_initialization();
   for (unsigned i = 0; i < count; i++) {
      assert(fixture.queue.kbase.targets[i] == 0);
      check_wrapper(&entries[i], i);
   }
   fixture.submit_calls++;
   return fixture.submit_failure ? -1 : 0;
}

int
kbase_csf_wait(struct kbase_csf_queue *const *queues, uint32_t count,
               const uint64_t *targets, uint64_t deadline)
{
   assert(count == PANVK_SUBQUEUE_COUNT && fixture.submit_calls == 1);
   assert(!fixture.submit_failure && deadline > os_time_get_nano());
   check_all_live();
   for (unsigned i = 0; i < count; i++)
      assert(queues[i] == &fixture.queues[i] && targets[i] == 1 &&
             fixture.queue.kbase.targets[i] == 1);
   fixture.wait_calls++;
   return fixture.wait_result;
}

static void
fixture_init(void)
{
   memset(&fixture, 0, sizeof(fixture));
   fixture.fail_stop = UINT32_MAX;
   fixture.bad_seqno = -1;
   fixture.wait_result = 1;
   fixture.kdev.ops = &kbase_kmod_ops;
   fixture.vm.dev = &fixture.kdev;
   fixture.device.kmod.dev = &fixture.kdev;
   fixture.device.kmod.vm = &fixture.vm;
   fixture.device.kbase.ctx = (void *)0x1000;
   fixture.device.kbase.csif_info = (struct drm_panthor_csif_info){
      .cs_reg_count = 128, .unpreserved_cs_reg_count = 4,
      .scoreboard_slot_count = 8,
   };
   fixture.device.csf.sb.count = 8;
   fixture.device.csf.sb.all_mask = 0xff;
   fixture.device.csf.sb.all_iters_mask = 0xf8;
   fixture.device.dump_region_size = fixture.dump_sizes;
   fixture.queue.vk.base.device = &fixture.device.vk;
   panvk_debug = 0;
   errno = 0;
}

static bool
cleanup(void)
{
   fixture.cleanup = true;
   fixture.stopped_mask = 0;
   return kbase_compute_cleanup(&fixture.queue);
}

static void
fixture_finish(void)
{
   assert(live_queue_mask() == 0 && !fixture.alias.live);
   for (unsigned i = 0; i < fixture.heap_count; i++)
      assert(!fixture.heaps[i].live);
   for (unsigned i = 0; i < fixture.allocation_count; i++)
      assert(!fixture.allocs[i].live && !fixture.allocs[i].host);
   unsigned released = fixture.releases;
   assert(cleanup() && fixture.releases == released);
}

static void
test_success(void)
{
   fixture_init();
   assert(kbase_compute_init(&fixture.queue) == VK_SUCCESS);
   assert(fixture.submit_calls == 1 && fixture.wait_calls == 1 && !fixture.lost_calls);
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      assert(fixture.queue.kbase.targets[i] == 1);
   assert(cleanup());
   fixture_finish();
}

static void
test_acquisition_failures(void)
{
   for (unsigned failure = 1; failure <= 13; failure++) {
      fixture_init();
      fixture.fail_acquire = failure;
      VkResult expected = failure <= 3 ? VK_ERROR_INITIALIZATION_FAILED :
                          failure == 10 ? VK_ERROR_OUT_OF_HOST_MEMORY :
                          VK_ERROR_OUT_OF_DEVICE_MEMORY;
      assert(kbase_compute_init(&fixture.queue) == expected);
      assert(fixture.acquired == failure && !fixture.submit_calls && !fixture.wait_calls);
      assert(cleanup());
      fixture_finish();
   }
   static const unsigned indirect[] = {5, 8, 9, 11, 12, 13};
   for (unsigned i = 0; i < ARRAY_SIZE(indirect); i++) {
      fixture_init();
      fixture.fail_acquire = indirect[i];
      fixture.host_oom = true;
      assert(kbase_compute_init(&fixture.queue) == VK_ERROR_OUT_OF_HOST_MEMORY);
      assert(errno == 0 && fixture.acquired == indirect[i]);
      assert(cleanup());
      fixture_finish();
   }
   for (unsigned failure = 1; failure <= 2; failure++) {
      fixture_init();
      fixture.fail_jit = failure;
      assert(kbase_compute_init(&fixture.queue) == VK_ERROR_INITIALIZATION_FAILED);
      assert(fixture.jit_calls == failure && fixture.acquired == (failure == 1 ? 0 : 3));
      assert(cleanup());
      fixture_finish();
   }
}

static void
test_publication_failures(void)
{
   for (unsigned id = 0; id < PANVK_SUBQUEUE_COUNT; id++) {
      fixture_init();
      fixture.bad_seqno = id;
      assert(kbase_compute_init(&fixture.queue) == VK_ERROR_INITIALIZATION_FAILED);
      assert(!fixture.submit_calls && !fixture.wait_calls);
      assert(cleanup());
      fixture_finish();
   }
   fixture_init();
   fixture.submit_failure = true;
   assert(kbase_compute_init(&fixture.queue) == VK_ERROR_DEVICE_LOST);
   assert(fixture.submit_calls == 1 && !fixture.wait_calls && fixture.lost_calls == 1);
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      assert(fixture.queue.kbase.targets[i] == 0);
   assert(cleanup());
   fixture_finish();
   for (int result = 0; result >= -1; result--) {
      fixture_init();
      fixture.wait_result = result;
      assert(kbase_compute_init(&fixture.queue) == VK_ERROR_DEVICE_LOST);
      assert(fixture.wait_calls == 1 && fixture.lost_calls == 1);
      for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
         assert(fixture.queue.kbase.targets[i] == 1);
      assert(cleanup());
      fixture_finish();
   }
}

static void
test_cleanup_failures(void)
{
   for (unsigned id = 0; id < PANVK_SUBQUEUE_COUNT; id++) {
      fixture_init();
      assert(kbase_compute_init(&fixture.queue) == VK_SUCCESS);
      fixture.fail_stop = id;
      assert(!cleanup() && fixture.device.kbase.quarantined);
      assert(fixture.stopped_mask == BITFIELD_MASK(PANVK_SUBQUEUE_COUNT));
      assert(fixture.releases == 0);
      check_all_live();
      fixture.fail_stop = UINT32_MAX;
      assert(cleanup());
      fixture_finish();
   }
   for (unsigned retired = 0; retired < 2; retired++) {
      fixture_init();
      assert(kbase_compute_init(&fixture.queue) == VK_SUCCESS);
      if (retired) {
         fixture.queue.kbase.retired_heap = kbase_graphics_heap_create(&fixture.queue);
         assert(fixture.queue.kbase.retired_heap);
      }
      fixture.fail_heap = retired ? fixture.queue.kbase.retired_heap : fixture.queue.kbase.heap;
      assert(!cleanup() && fixture.device.kbase.quarantined);
      assert(fixture.releases == 0);
      check_all_live();
      fixture.fail_heap = NULL;
      assert(cleanup());
      fixture_finish();
   }
   fixture_init();
   assert(kbase_compute_init(&fixture.queue) == VK_SUCCESS);
   fixture.fail_alias = true;
   assert(!cleanup() && fixture.device.kbase.quarantined);
   assert(fixture.releases == 1 && !fixture.heaps[0].live && fixture.alias.live);
   assert(live_queue_mask() == BITFIELD_MASK(PANVK_SUBQUEUE_COUNT));
   for (unsigned i = 0; i < fixture.allocation_count; i++)
      assert(fixture.allocs[i].live);
   fixture.fail_alias = false;
   assert(cleanup());
   fixture_finish();
}

static void
test_register_save(void)
{
   for (unsigned id = 0; id < PANVK_SUBQUEUE_COUNT; id++) {
      fixture_init();
      fixture.dump_sizes[id] = 4096;
      assert(kbase_compute_init(&fixture.queue) == VK_SUCCESS);
      assert(fixture.acquired == 14 && fixture.queue.subqueues[id].regs_save.bo);
      assert(cleanup());
      fixture_finish();
      for (unsigned host = 0; host < 2; host++) {
         fixture_init();
         fixture.dump_sizes[id] = 4096;
         fixture.fail_acquire = 11 + id;
         fixture.host_oom = host;
         assert(kbase_compute_init(&fixture.queue) == (host ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY));
         assert(!fixture.submit_calls && !fixture.wait_calls);
         assert(cleanup());
         fixture_finish();
      }
   }
}

static void
test_gates(void)
{
   fixture_init();
   panvk_debug = PANVK_DEBUG_TRACE;
   assert(kbase_compute_init(&fixture.queue) == VK_ERROR_FEATURE_NOT_PRESENT);
   assert(!fixture.jit_calls && !fixture.acquired);
   assert(cleanup());
   fixture_finish();
   fixture_init();
   assert(cleanup());
   fixture_finish();
}

int
main(void)
{
   test_success();
   test_acquisition_failures();
   test_publication_failures();
   test_cleanup_failures();
   test_register_save();
   test_gates();
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
