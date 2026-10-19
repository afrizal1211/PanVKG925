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

#include <string.h>

#include "genxml/cs_builder.h"
#include "panvk_kbase_csf_encode.h"

#define KBASE_ENCODE_NAME_(arch) panvk_v##arch##_kbase_csf_encode
#define KBASE_ENCODE_NAME(arch) KBASE_ENCODE_NAME_(arch)

static void
kbase_move64_to(struct cs_builder *b, struct cs_index dest, uint64_t value)
{
   if (value < (1ull << 48)) {
      cs_move48_to(b, dest, value);
      return;
   }
   struct cs_index low = {.type = CS_INDEX_REGISTER, .size = 1, .reg = dest.reg};
   struct cs_index high = {.type = CS_INDEX_REGISTER, .size = 1, .reg = dest.reg + 1};
   cs_move32_to(b, low, value);
   cs_move32_to(b, high, value >> 32);
}

static bool
panvk_kbase_csf_encode(const struct panvk_kbase_csf_encode_info *info,
                       uint64_t *commands, uint32_t capacity_bytes,
                       uint32_t *size_bytes)
{
   uint64_t temporary[128];

   if (size_bytes)
      *size_bytes = 0;

   if (!info || !commands || !size_bytes || capacity_bytes < sizeof(uint64_t) ||
       (capacity_bytes & (sizeof(uint64_t) - 1)) ||
       info->register_count < 8 || info->register_count > UINT8_MAX ||
       (info->register_count & 1) ||
       info->unpreserved_count < 4 ||
       info->unpreserved_count > info->register_count ||
       info->scoreboard_count < 4 || info->scoreboard_count > CS_MAX_SB_COUNT ||
       info->ls_slot >= info->scoreboard_count ||
       info->flush_slot >= info->scoreboard_count ||
       info->deferred_slot >= info->scoreboard_count ||
       info->iterator_slot >= info->scoreboard_count ||
       info->deferred_slot == info->ls_slot ||
       info->deferred_slot == info->flush_slot ||
       info->deferred_slot == info->iterator_slot ||
       info->iterator_slot == info->ls_slot ||
       info->iterator_slot == info->flush_slot ||
       !info->iterator_mask ||
       (info->iterator_mask & ((1u << info->ls_slot) |
                               (1u << info->flush_slot) |
                               (1u << info->deferred_slot))) ||
       (info->iterator_mask & ~((1u << info->scoreboard_count) - 1)) ||
       !(info->iterator_mask & (1u << info->iterator_slot)) ||
       (info->context_register & 1) ||
       info->context_register >
          info->register_count - info->unpreserved_count ||
       info->register_count - info->unpreserved_count - info->context_register < 2 ||
       !info->resource_mask || (info->resource_mask & ~0xfu) ||
       !info->context_address ||
       (info->context_address & 0xf) ||
       (info->heap_address && (info->heap_address & 0xf)) ||
       !info->stream_address || (info->stream_address & 7) ||
       !info->stream_size || (info->stream_size & 7) ||
       !info->seqno_address || (info->seqno_address & 0xf))
      return false;

   struct cs_buffer buffer = {
      .cpu = temporary,
      .gpu = 0,
      .capacity = ARRAY_SIZE(temporary),
   };
   struct cs_builder_conf conf = {
      .nr_registers = info->register_count,
      .nr_kernel_registers = MAX2(info->unpreserved_count, 4),
      .ls_sb_slot = info->ls_slot,
   };
   struct cs_builder b;
   cs_builder_init(&b, &conf, buffer);

#if PAN_ARCH >= 11
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_ENDPOINT,
                      info->iterator_slot);
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_MASK_WAIT,
                      1u << info->iterator_slot);
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_OTHER,
                      info->ls_slot);
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_SEL_DEFERRED,
                      info->deferred_slot);
   cs_set_state_imm32(&b, MALI_CS_SET_STATE_TYPE_SB_MASK_STREAM,
                      info->iterator_mask & ~(1u << info->iterator_slot));
#else
   cs_set_scoreboard_entry(&b, info->iterator_slot, info->ls_slot);
#endif

   cs_req_res(&b, info->resource_mask);
   kbase_move64_to(&b, cs_reg64(&b, info->context_register), info->context_address);

   uint8_t scratch_reg = info->register_count - 4;
   struct cs_index addr64 = {
      .type = CS_INDEX_REGISTER,
      .size = 2,
      .reg = scratch_reg,
   };
   struct cs_index val32 = {
      .type = CS_INDEX_REGISTER,
      .size = 1,
      .reg = scratch_reg + 2,
   };
   struct cs_index val64 = {
      .type = CS_INDEX_REGISTER,
      .size = 2,
      .reg = scratch_reg + 2,
   };

   if (info->heap_address) {
      kbase_move64_to(&b, addr64, info->heap_address);
      cs_heap_set(&b, addr64);
   }

   cs_move32_to(&b, val32, info->flush_id);
   cs_flush_caches(&b, MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                   MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE,
                   MALI_CS_OTHER_FLUSH_MODE_INVALIDATE, val32,
                   cs_defer(0, info->flush_slot));
   cs_wait_slot(&b, info->flush_slot);

   kbase_move64_to(&b, addr64, info->stream_address);
   cs_move32_to(&b, val32, info->stream_size);
   cs_call(&b, addr64, val32);

   kbase_move64_to(&b, addr64, info->seqno_address);
   cs_wait_slots(&b, (1u << info->scoreboard_count) - 1);
   kbase_move64_to(&b, val64, 1);
   cs_sync64_add(&b, true, MALI_CS_SYNC_SCOPE_SYSTEM, val64, addr64,
                 cs_defer(0, info->deferred_slot));

   /* Optional probe writeback.  A plain SYNC_ADD64 has no condition and never
    * blocks, so this cannot hang the queue; if the address is not mapped the
    * MMU fault is reported through the usual error path.
    */
   if (info->probe_address) {
      kbase_move64_to(&b, addr64, info->probe_address);
      kbase_move64_to(&b, val64, info->probe_value);
      cs_sync64_add(&b, true, MALI_CS_SYNC_SCOPE_SYSTEM, val64, addr64,
                    cs_defer(0, info->deferred_slot));
   }

   cs_error_barrier(&b);
   cs_end(&b);

   bool valid = cs_is_valid(&b);
   uint32_t size = valid ? cs_root_chunk_size(&b) : 0;
   cs_builder_fini(&b);

   if (!valid || size > capacity_bytes || size > sizeof(temporary))
      return false;

   memcpy(commands, temporary, size);
   *size_bytes = size;
   return true;
}

bool
KBASE_ENCODE_NAME(PAN_ARCH)(const struct panvk_kbase_csf_encode_info *info,
                            uint64_t *commands, uint32_t capacity_bytes,
                            uint32_t *size_bytes)
{
   return panvk_kbase_csf_encode(info, commands, capacity_bytes, size_bytes);
}
