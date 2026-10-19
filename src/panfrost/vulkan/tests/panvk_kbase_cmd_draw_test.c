#include "drm-uapi/panthor_drm.h"
#include "genxml/cs_builder.h"
#include "panvk_cmd_buffer.h"
#include "panvk_device.h"
#include "panvk_physical_device.h"
#include "panvk_queue.h"
#include "kmod/pan_kmod.h"
#include "pan_fb.h"
#include "pan_pool.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

uint64_t panvk_debug;
static struct u_trace_context trace_context;
const struct pan_kmod_ops kbase_kmod_ops = {0};
static const struct pan_kmod_ops drm_ops = {0};

struct pan_ptr
pan_pool_alloc_aligned(struct pan_pool *pool, size_t size, unsigned alignment)
{
   abort();
}

#include "../csf/panvk_vX_cmd_draw.c"

static void
init_cmd_buffer(struct panvk_cmd_buffer *cmdbuf, struct panvk_device *device,
                struct panvk_physical_device *phys_dev,
                struct pan_kmod_dev *kdev, uint64_t *vt_cs, uint64_t *frag_cs,
                size_t capacity)
{
   memset(cmdbuf, 0, sizeof(*cmdbuf));
   memset(device, 0, sizeof(*device));
   memset(phys_dev, 0, sizeof(*phys_dev));
   memset(kdev, 0, sizeof(*kdev));

   kdev->ops = &kbase_kmod_ops;
   kdev->props.tiler_features = (8 << 8) | 9;
   phys_dev->kmod.dev = kdev;
   phys_dev->csf.tiler.chunk_size = 1024 * 1024;
   device->kmod.dev = kdev;
   device->csf.sb.count = 8;
   device->csf.sb.all_mask = 0xff;
   device->csf.sb.all_iters_mask = 0xf8;
   device->vk.physical = &phys_dev->vk;
   cmdbuf->vk.base.device = &device->vk;
   cmdbuf->vk.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cmdbuf->flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

   struct cs_builder_conf conf = {
      .nr_registers = 128,
      .nr_kernel_registers = 4,
      .ls_sb_slot = SB_ID(LS),
   };
   struct cs_buffer buffers[PANVK_SUBQUEUE_COUNT] = {
      [PANVK_SUBQUEUE_VERTEX_TILER] = {
         .cpu = vt_cs,
         .gpu = 0x100000,
         .capacity = capacity,
      },
      [PANVK_SUBQUEUE_FRAGMENT] = {
         .cpu = frag_cs,
         .gpu = 0x200000,
         .capacity = capacity,
      },
   };
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      cs_builder_init(&cmdbuf->state.cs[i].builder, &conf, buffers[i]);
      cmdbuf->utrace.uts[i].utctx = &trace_context;
   }

   cmdbuf->state.gfx.render.first_provoking_vertex = U_TRISTATE_YES;
   cmdbuf->state.gfx.render.layer_count = 1;
   cmdbuf->state.gfx.render.fb.layout = (struct pan_fb_layout){
      .width_px = 64,
      .height_px = 64,
      .sample_count = 1,
      .tile_size_px = 16,
      .rt_count = 1,
      .tiling_area_px = { .min_x = 0, .min_y = 0, .max_x = 63, .max_y = 63 },
   };
}

static size_t
count_opcode(const uint64_t *commands, size_t command_count,
             enum mali_cs_opcode opcode)
{
   size_t count = 0;
   for (size_t i = 0; i < command_count; i++)
      count += ((commands[i] >> 56) == opcode);
   return count;
}

static size_t
find_opcode(const uint64_t *commands, size_t command_count,
            enum mali_cs_opcode opcode, size_t from)
{
   for (size_t i = from; i < command_count; i++) {
      if ((commands[i] >> 56) == opcode)
         return i;
   }
   return SIZE_MAX;
}

static size_t
count_heap_operation(const uint64_t *commands, size_t command_count,
                     enum mali_cs_heap_operation operation)
{
   size_t count = 0;
   for (size_t i = 0; i < command_count; i++) {
      if ((commands[i] >> 56) != MALI_CS_OPCODE_HEAP_OPERATION)
         continue;
      struct MALI_CS_HEAP_OPERATION packet;
      MALI_CS_HEAP_OPERATION_unpack(
         (const struct mali_cs_heap_operation_packed *)&commands[i], &packet);
      count += packet.operation == operation;
   }
   return count;
}

static size_t
find_heap_operation(const uint64_t *commands, size_t command_count,
                    enum mali_cs_heap_operation operation, size_t from)
{
   for (size_t i = from; i < command_count; i++) {
      if ((commands[i] >> 56) != MALI_CS_OPCODE_HEAP_OPERATION)
         continue;
      struct MALI_CS_HEAP_OPERATION packet;
      MALI_CS_HEAP_OPERATION_unpack(
         (const struct mali_cs_heap_operation_packed *)&commands[i], &packet);
      if (packet.operation == operation)
         return i;
   }
   return SIZE_MAX;
}

struct reg_definition {
   enum mali_cs_opcode opcode;
   size_t index;
   uint64_t immediate;
   unsigned source;
   unsigned base;
   unsigned mask;
   int offset;
};

static struct reg_definition
last_definition(const uint64_t *commands, size_t before, unsigned reg)
{
   while (before) {
      uint64_t word = commands[--before];
      switch (word >> 56) {
      case MALI_CS_OPCODE_MOVE32: {
         struct MALI_CS_MOVE32 p;
         MALI_CS_MOVE32_unpack((const struct mali_cs_move32_packed *)&word, &p);
         if (p.destination == reg)
            return (struct reg_definition){.opcode = p.opcode, .index = before, .immediate = p.immediate};
         break;
      }
      case MALI_CS_OPCODE_MOVE48: {
         struct MALI_CS_MOVE48 p;
         MALI_CS_MOVE48_unpack((const struct mali_cs_move48_packed *)&word, &p);
         if (p.destination == reg)
            return (struct reg_definition){.opcode = p.opcode, .index = before, .immediate = p.immediate};
         break;
      }
      case MALI_CS_OPCODE_ARITH_IMM64: {
         struct MALI_CS_ADD_IMM64 p;
         MALI_CS_ADD_IMM64_unpack((const struct mali_cs_add_imm64_packed *)&word, &p);
         if (p.destination == reg)
            return (struct reg_definition){.opcode = p.opcode, .index = before, .immediate = p.immediate,
                                          .source = p.source};
         break;
      }
      case MALI_CS_OPCODE_LOAD_MULTIPLE: {
         struct MALI_CS_LOAD_MULTIPLE p;
         MALI_CS_LOAD_MULTIPLE_unpack((const struct mali_cs_load_multiple_packed *)&word, &p);
         if (reg >= p.base_register && reg < p.base_register + 16 &&
             (p.mask & (1u << (reg - p.base_register))))
            return (struct reg_definition){.opcode = p.opcode, .index = before, .source = p.address,
                                          .base = p.base_register, .mask = p.mask,
                                          .offset = p.offset};
         break;
      }
      default:
         break;
      }
   }
   return (struct reg_definition){0};
}

static void
check_context_load(const uint64_t *commands, size_t before, unsigned reg, unsigned offset)
{
   struct reg_definition def = last_definition(commands, before, reg);
   assert(def.opcode == MALI_CS_OPCODE_LOAD_MULTIPLE);
   assert(def.source == PANVK_CS_REG_SUBQUEUE_CTX_START && def.base == reg);
   assert(def.offset == offset && (def.mask & 3) == 3);
}

static enum mali_cs_sync_scope
expected_signal_scope(bool kbase)
{
   return kbase ? MALI_CS_SYNC_SCOPE_SYSTEM : MALI_CS_SYNC_SCOPE_CSG;
}

static void
check_sync_add(const uint64_t *commands, size_t index, unsigned offset,
               bool kbase)
{
   struct MALI_CS_SYNC_ADD64 p;
   MALI_CS_SYNC_ADD64_unpack((const struct mali_cs_sync_add64_packed *)&commands[index], &p);
   assert(p.error_propagate && p.scope == expected_signal_scope(kbase));
   assert(p.defer_mode == MALI_CS_DEFER_MODE_DEFER_INDIRECT);
   struct reg_definition value = last_definition(commands, index, p.data);
   assert(value.opcode == MALI_CS_OPCODE_MOVE48 && value.immediate == 1);
   if (offset) {
      struct reg_definition addr = last_definition(commands, index, p.address);
      assert(addr.opcode == MALI_CS_OPCODE_ARITH_IMM64 && addr.source == p.address);
      assert(addr.immediate == offset);
      check_context_load(commands, addr.index, addr.source,
                         offsetof(struct panvk_cs_subqueue_context, syncobjs));
   } else {
      check_context_load(commands, index, p.address,
                         offsetof(struct panvk_cs_subqueue_context, syncobjs));
   }
}

static void
check_fragment_operands(const uint64_t *commands, size_t count, bool kbase,
                        unsigned layers, size_t wait, size_t run, size_t release)
{
   struct MALI_CS_SYNC_WAIT64 sync_wait;
   MALI_CS_SYNC_WAIT64_unpack((const struct mali_cs_sync_wait64_packed *)&commands[wait], &sync_wait);
   assert(!sync_wait.error_reject && sync_wait.condition == MALI_CS_CONDITION_GREATER);
   check_context_load(commands, wait, sync_wait.address,
                      offsetof(struct panvk_cs_subqueue_context, syncobjs));
   struct reg_definition reference = last_definition(commands, wait, sync_wait.data);
   assert(reference.opcode == MALI_CS_OPCODE_ARITH_IMM64);
   assert(reference.source == PANVK_CS_REG_PROGRESS_SEQNO_START && reference.immediate == 1);
   struct MALI_CS_RUN_FRAGMENT fragment;
   MALI_CS_RUN_FRAGMENT_unpack((const struct mali_cs_run_fragment_packed *)&commands[run], &fragment);
   assert(!fragment.enable_tem && fragment.tile_order == MALI_TILE_RENDER_ORDER_Z_ORDER);
   struct MALI_CS_SYNC_ADD32 give_back;
   MALI_CS_SYNC_ADD32_unpack((const struct mali_cs_sync_add32_packed *)&commands[release], &give_back);
   assert(give_back.error_propagate &&
          give_back.scope == expected_signal_scope(kbase));
   assert(give_back.defer_mode == MALI_CS_DEFER_MODE_DEFER_INDIRECT);
   check_context_load(commands, release, give_back.address,
                      offsetof(struct panvk_cs_subqueue_context, render.desc_ringbuf.syncobj));
   struct reg_definition bytes = last_definition(commands, release, give_back.data);
   unsigned expected = get_fbd_size(false, 1) * layers +
                       DIV_ROUND_UP(layers, MAX_LAYERS_PER_TILER_DESC) * pan_size(TILER_CONTEXT);
   assert(bytes.opcode == MALI_CS_OPCODE_MOVE32 && bytes.immediate == expected);
   size_t final_sync = find_opcode(commands, count, MALI_CS_OPCODE_SYNC_ADD64, release + 1);
   assert(final_sync != SIZE_MAX);
   check_sync_add(commands, final_sync,
                  PANVK_SUBQUEUE_FRAGMENT * sizeof(struct panvk_cs_sync64), kbase);
   unsigned handlers = 0, oom_stores = 0, pointer_stores = 0;
   for (size_t i = 0; i < count; i++) {
      switch (commands[i] >> 56) {
      case MALI_CS_OPCODE_SET_EXCEPTION_HANDLER: {
         struct MALI_CS_SET_EXCEPTION_HANDLER p;
         MALI_CS_SET_EXCEPTION_HANDLER_unpack((const struct mali_cs_set_exception_handler_packed *)&commands[i], &p);
         assert(!kbase && p.exception_type == MALI_CS_EXCEPTION_TYPE_TILER_OOM);
         struct reg_definition address = last_definition(commands, i, p.address);
         struct reg_definition length = last_definition(commands, i, p.length);
         assert(address.opcode == MALI_CS_OPCODE_MOVE48 && length.opcode == MALI_CS_OPCODE_MOVE32);
         assert(address.immediate == (handlers ? 0 : 0x800000));
         assert(length.immediate == (handlers ? 0 : 256));
         assert(handlers ? i > wait : i < wait);
         handlers++;
         break;
      }
      case MALI_CS_OPCODE_STORE_MULTIPLE: {
         struct MALI_CS_STORE_MULTIPLE p;
         MALI_CS_STORE_MULTIPLE_unpack((const struct mali_cs_store_multiple_packed *)&commands[i], &p);
         if (p.address == PANVK_CS_REG_SUBQUEUE_CTX_START) {
            unsigned start = offsetof(struct panvk_cs_subqueue_context, tiler_oom_ctx);
            unsigned end = start + sizeof(((struct panvk_cs_subqueue_context *)0)->tiler_oom_ctx);
            oom_stores += p.offset >= start && p.offset < end;
            if (p.offset == offsetof(struct panvk_cs_subqueue_context, render.desc_ringbuf.ptr)) {
               assert(i > final_sync && p.mask == 7);
               struct reg_definition previous = last_definition(commands, i, p.base_register);
               assert(previous.opcode == MALI_CS_OPCODE_LOAD_MULTIPLE);
               assert(previous.source == PANVK_CS_REG_SUBQUEUE_CTX_START);
               assert(previous.offset == p.offset);
               unsigned increments = 0;
               for (size_t j = previous.index + 1; j < i; j++) {
                  if (commands[j] >> 56 != MALI_CS_OPCODE_ARITH_IMM32)
                     continue;
                  struct MALI_CS_ADD_IMM32 add;
                  MALI_CS_ADD_IMM32_unpack((const struct mali_cs_add_imm32_packed *)&commands[j], &add);
                  if (add.immediate == expected && add.source == add.destination &&
                      (add.destination == p.base_register || add.destination == p.base_register + 2))
                     increments++;
               }
               assert(increments == 2);
               pointer_stores++;
            }
         }
         break;
      }
      case MALI_CS_OPCODE_FINISH_FRAGMENT: {
         struct MALI_CS_FINISH_FRAGMENT p;
         MALI_CS_FINISH_FRAGMENT_unpack((const struct mali_cs_finish_fragment_packed *)&commands[i], &p);
         assert(!kbase && p.increment_fragment_completed == (layers <= MAX_LAYERS_PER_TILER_DESC));
         assert(p.defer_mode == MALI_CS_DEFER_MODE_DEFER_INDIRECT);
         assert(i > run && i < release);
         break;
      }
      default:
         break;
      }
   }
   assert(pointer_stores == 1);
   assert(handlers == (kbase ? 0 : 2));
   assert(kbase ? oom_stores == 0 : oom_stores > 0);
}

static void
check_command_streams(const uint64_t *vt, size_t vt_count,
                      const uint64_t *frag, size_t frag_count, bool kbase,
                      unsigned layers)
{
   size_t finish_tiling = find_opcode(vt, vt_count, MALI_CS_OPCODE_FINISH_TILING, 0);
   size_t sync_add = find_opcode(vt, vt_count, MALI_CS_OPCODE_SYNC_ADD64, 0);
   size_t vt_end = find_heap_operation(vt, vt_count,
      MALI_CS_HEAP_OPERATION_VERTEX_TILER_COMPLETED, 0);
   size_t sync_wait = find_opcode(frag, frag_count, MALI_CS_OPCODE_SYNC_WAIT64, 0);
   size_t run_fragment = find_opcode(frag, frag_count, MALI_CS_OPCODE_RUN_FRAGMENT, 0);
   size_t finish_fragment = find_opcode(frag, frag_count, MALI_CS_OPCODE_FINISH_FRAGMENT, 0);
   size_t frag_end = find_heap_operation(frag, frag_count,
      MALI_CS_HEAP_OPERATION_FRAGMENT_COMPLETED, 0);
   size_t release_sync = find_opcode(frag, frag_count, MALI_CS_OPCODE_SYNC_ADD32, 0);
   size_t exception_count = count_opcode(frag, frag_count,
                                         MALI_CS_OPCODE_SET_EXCEPTION_HANDLER);

   assert(finish_tiling != SIZE_MAX && sync_add != SIZE_MAX);
   assert(sync_wait != SIZE_MAX && run_fragment != SIZE_MAX);
   assert(release_sync != SIZE_MAX);
   assert(finish_tiling < sync_add);
   assert(sync_wait < run_fragment && run_fragment < release_sync);

   if (kbase) {
      assert(vt_end == SIZE_MAX && finish_fragment == SIZE_MAX);
      assert(frag_end == SIZE_MAX);
      assert(exception_count == 0);
   } else {
      assert(vt_end != SIZE_MAX && vt_end < sync_add);
      assert(finish_fragment != SIZE_MAX);
      assert(run_fragment < finish_fragment && finish_fragment < release_sync);
      assert((frag_end != SIZE_MAX) == (layers > MAX_LAYERS_PER_TILER_DESC));
      if (layers > MAX_LAYERS_PER_TILER_DESC)
         assert(finish_fragment < frag_end && frag_end < release_sync);
      assert(exception_count == 2);
   }
   assert(count_heap_operation(vt, vt_count,
      MALI_CS_HEAP_OPERATION_VERTEX_TILER_STARTED) == 1);
   check_sync_add(vt, sync_add, 0, kbase);
   check_fragment_operands(frag, frag_count, kbase, layers,
                           sync_wait, run_fragment, release_sync);
   assert(count_opcode(vt, vt_count, MALI_CS_OPCODE_FINISH_TILING) == 1);
   assert(count_opcode(vt, vt_count, MALI_CS_OPCODE_SYNC_ADD64) == 1);
   assert(count_opcode(frag, frag_count, MALI_CS_OPCODE_SYNC_WAIT64) == 1);
   assert(count_opcode(frag, frag_count, MALI_CS_OPCODE_RUN_FRAGMENT) == 1);
   assert(count_opcode(frag, frag_count, MALI_CS_OPCODE_SYNC_ADD32) == 1);
   assert(count_opcode(frag, frag_count, MALI_CS_OPCODE_SYNC_ADD64) == 1);
}

struct recorded_streams {
   uint64_t vt[2048];
   uint64_t fragment[4096];
   size_t vt_words;
   size_t fragment_words;
};

static void
run_case(bool kbase, unsigned layers, struct recorded_streams *recorded)
{
   struct panvk_cmd_buffer cmdbuf;
   struct panvk_device device;
   struct panvk_physical_device phys_dev;
   struct pan_kmod_dev kdev;
   uint64_t vt_cs[2048] = {0};
   uint64_t frag_cs[4096] = {0};

   init_cmd_buffer(&cmdbuf, &device, &phys_dev, &kdev, vt_cs, frag_cs,
                   ARRAY_SIZE(vt_cs));
   struct panvk_priv_bo handlers = {.addr.dev = 0x800000};
   if (!kbase) {
      kdev.ops = &drm_ops;
      device.tiler_oom.handlers_bo = &handlers;
      device.tiler_oom.handler_stride = 256;
   }
   assert(panvk_device_uses_kbase(&device) == kbase);
   cmdbuf.state.gfx.render.layer_count = layers;

   assert(get_tiler_desc(&cmdbuf) == VK_SUCCESS);
   assert(cmdbuf.state.gfx.render.tiler == UINT64_C(0xdeadbeefdeadbeef));
   struct cs_builder *vt_builder = panvk_get_cs_builder(&cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
   unsigned tiler_words = vt_builder->cur_chunk.pos;
   assert(get_tiler_desc(&cmdbuf) == VK_SUCCESS && vt_builder->cur_chunk.pos == tiler_words);
   flush_tiling(&cmdbuf);
   assert(issue_fragment_jobs(&cmdbuf) == VK_SUCCESS);

   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct cs_builder *b = panvk_get_cs_builder(&cmdbuf, i);
      assert(cs_is_valid(b));
      cs_end(b);
      assert(cs_is_valid(b));
   }
   size_t vt_count = cs_root_chunk_size(panvk_get_cs_builder(&cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER)) / 8;
   size_t frag_count = cs_root_chunk_size(panvk_get_cs_builder(&cmdbuf, PANVK_SUBQUEUE_FRAGMENT)) / 8;
   assert(vt_count && vt_count < ARRAY_SIZE(vt_cs));
   assert(frag_count && frag_count < ARRAY_SIZE(frag_cs));
   check_command_streams(vt_cs, vt_count, frag_cs, frag_count, kbase, layers);
   assert(cmdbuf.state.cs[PANVK_SUBQUEUE_VERTEX_TILER].relative_sync_point == 1);
   assert(cmdbuf.state.cs[PANVK_SUBQUEUE_FRAGMENT].relative_sync_point == 1);
   memset(recorded, 0, sizeof(*recorded));
   memcpy(recorded->vt, vt_cs, vt_count * sizeof(*vt_cs));
   memcpy(recorded->fragment, frag_cs, frag_count * sizeof(*frag_cs));
   recorded->vt_words = vt_count;
   recorded->fragment_words = frag_count;

   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
      cs_builder_fini(panvk_get_cs_builder(&cmdbuf, i));
}

static void
test_without_tiling(void)
{
   for (unsigned kbase = 0; kbase < 2; kbase++) {
      struct panvk_cmd_buffer cmdbuf;
      struct panvk_device device;
      struct panvk_physical_device phys_dev;
      struct pan_kmod_dev kdev;
      uint64_t vt[2048] = {0}, fragment[2048] = {0};
      init_cmd_buffer(&cmdbuf, &device, &phys_dev, &kdev, vt, fragment, ARRAY_SIZE(vt));
      struct panvk_priv_bo handlers = {.addr.dev = 0x800000};
      if (!kbase) {
         kdev.ops = &drm_ops;
         device.tiler_oom.handlers_bo = &handlers;
         device.tiler_oom.handler_stride = 256;
      }
      flush_tiling(&cmdbuf);
      struct cs_builder *vt_builder = panvk_get_cs_builder(&cmdbuf, PANVK_SUBQUEUE_VERTEX_TILER);
      assert(vt_builder->cur_chunk.pos == 0);
      assert(cmdbuf.state.cs[PANVK_SUBQUEUE_VERTEX_TILER].relative_sync_point == 0);
      assert(issue_fragment_jobs(&cmdbuf) == VK_SUCCESS);
      struct cs_builder *b = panvk_get_cs_builder(&cmdbuf, PANVK_SUBQUEUE_FRAGMENT);
      cs_end(b);
      assert(cs_is_valid(b));
      size_t count = cs_root_chunk_size(b) / 8;
      assert(count && count < ARRAY_SIZE(fragment));
      assert(count_opcode(fragment, count, MALI_CS_OPCODE_RUN_FRAGMENT) == 1);
      assert(count_opcode(fragment, count, MALI_CS_OPCODE_SYNC_ADD64) == 1);
      assert(count_opcode(fragment, count, MALI_CS_OPCODE_FINISH_FRAGMENT) == 0);
      assert(count_opcode(fragment, count, MALI_CS_OPCODE_SYNC_ADD32) == 0);
      assert(count_heap_operation(fragment, count, MALI_CS_HEAP_OPERATION_FRAGMENT_COMPLETED) == 0);
      assert(cmdbuf.state.cs[PANVK_SUBQUEUE_FRAGMENT].relative_sync_point == 1);
      for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++)
         cs_builder_fini(panvk_get_cs_builder(&cmdbuf, i));
   }
}

int
main(void)
{
   test_without_tiling();
   const unsigned layer_counts[] = {1, MAX_LAYERS_PER_TILER_DESC + 1};
   for (unsigned kbase = 0; kbase < 2; kbase++) {
      for (unsigned i = 0; i < ARRAY_SIZE(layer_counts); i++) {
         struct recorded_streams first, second;
         run_case(kbase, layer_counts[i], &first);
         run_case(kbase, layer_counts[i], &second);
         assert(first.vt_words == second.vt_words);
         assert(first.fragment_words == second.fragment_words);
         assert(memcmp(first.vt, second.vt, first.vt_words * sizeof(*first.vt)) == 0);
         assert(memcmp(first.fragment, second.fragment,
                       first.fragment_words * sizeof(*first.fragment)) == 0);
      }
   }
   return 0;
}
