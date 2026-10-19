#include "panvk_kbase_csf_encode.h"
#include "genxml/gen_macros.h"

#include <assert.h>
#include <string.h>

#define ENCODE_NAME_(arch) panvk_v##arch##_kbase_csf_encode
#define ENCODE_NAME(arch) ENCODE_NAME_(arch)

int
main(void)
{
   struct panvk_kbase_csf_encode_info info = {
      .register_count = 96,
      .unpreserved_count = 4,
      .scoreboard_count = 8,
      .ls_slot = 0,
      .flush_slot = 2,
      .deferred_slot = 1,
      .iterator_slot = 3,
      .iterator_mask = 0xf8,
      .context_register = 0,
      .resource_mask = 1,
      .context_address = 0x100000,
      .heap_address = 0x200000,
      .stream_address = 0x300000,
      .stream_size = 256,
      .flush_id = 17,
      .seqno_address = 0x400000,
   };
   uint64_t commands[128];
   uint32_t size = 0;
   assert(ENCODE_NAME(PAN_ARCH)(&info, commands, sizeof(commands), &size));
   assert(size > 0 && size <= sizeof(commands) && !(size & 7));
   unsigned flush_index = UINT32_MAX, call_index = UINT32_MAX;
   unsigned all_wait_index = UINT32_MAX, sync_index = UINT32_MAX;
   unsigned flush_wait_index = UINT32_MAX;
   unsigned state_count = 0;
   for (unsigned i = 0; i < size / 8; i++) {
      uint8_t opcode = commands[i] >> 56;
#if PAN_ARCH >= 11
      if (opcode == MALI_CS_OPCODE_SET_STATE_IMM32) {
         struct MALI_CS_SET_STATE_IMM32 packet;
         MALI_CS_SET_STATE_IMM32_unpack(
            (const struct mali_cs_set_state_imm32_packed *)&commands[i], &packet);
         if (packet.state == MALI_CS_SET_STATE_TYPE_SB_MASK_WAIT)
            assert(packet.value == (1u << info.iterator_slot));
         if (packet.state == MALI_CS_SET_STATE_TYPE_SB_MASK_STREAM)
            assert(packet.value == (info.iterator_mask & ~(1u << info.iterator_slot)));
         state_count++;
      }
#endif
      if (opcode == MALI_CS_OPCODE_FLUSH_CACHE2) {
         struct MALI_CS_FLUSH_CACHE2 packet;
         MALI_CS_FLUSH_CACHE2_unpack((const struct mali_cs_flush_cache2_packed *)&commands[i], &packet);
         assert(packet.wait_mask == 0 && packet.signal_slot == info.flush_slot);
         assert(packet.l2_flush_mode == MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE);
         assert(packet.lsc_flush_mode == MALI_CS_FLUSH_MODE_CLEAN_AND_INVALIDATE);
         assert(packet.other_flush_mode == MALI_CS_OTHER_FLUSH_MODE_INVALIDATE);
         assert(packet.latest_flush_id == 94);
         flush_index = i;
      }
      if (opcode == MALI_CS_OPCODE_CALL)
         call_index = i;
      if (opcode == MALI_CS_OPCODE_WAIT) {
         struct MALI_CS_WAIT packet;
         MALI_CS_WAIT_unpack((const struct mali_cs_wait_packed *)&commands[i], &packet);
         if (packet.wait_mask == 0xff)
            all_wait_index = i;
         if (packet.wait_mask == (1u << info.flush_slot))
            flush_wait_index = i;
      }
      if (opcode == MALI_CS_OPCODE_SYNC_ADD64) {
         struct MALI_CS_SYNC_ADD64 packet;
         MALI_CS_SYNC_ADD64_unpack((const struct mali_cs_sync_add64_packed *)&commands[i], &packet);
         assert(packet.error_propagate && packet.scope == MALI_CS_SYNC_SCOPE_SYSTEM);
         assert(packet.wait_mask == 0 && packet.signal_slot == 0);
         assert(packet.address == 92 && packet.data == 94);
         sync_index = i;
      }
   }
   assert(flush_index < flush_wait_index && flush_wait_index < call_index);
   assert(call_index < all_wait_index && all_wait_index < sync_index);
   assert(sync_index + 1 == size / 8 - 1);
   assert((commands[size / 8 - 1] >> 56) == MALI_CS_OPCODE_ERROR_BARRIER);
#if PAN_ARCH >= 11
   assert(state_count == 5);
#else
   assert(state_count == 0);
#endif
   memset(commands, 0xa5, sizeof(commands));
   uint64_t before[128];
   memcpy(before, commands, sizeof(before));
   assert(!ENCODE_NAME(PAN_ARCH)(&info, commands, 8, &size) && size == 0);
   assert(memcmp(before, commands, sizeof(before)) == 0);
   info.deferred_slot = info.flush_slot;
   assert(!ENCODE_NAME(PAN_ARCH)(&info, commands, sizeof(commands), &size));
   info.deferred_slot = 1;
   info.context_register = UINT32_MAX;
   assert(!ENCODE_NAME(PAN_ARCH)(&info, commands, sizeof(commands), &size));
   info.context_register = 0;
   info.register_count = 95;
   assert(!ENCODE_NAME(PAN_ARCH)(&info, commands, sizeof(commands), &size));
   info.register_count = 96;
   info.stream_address = UINT64_C(0x1000000000030000);
   info.seqno_address = UINT64_C(0x1000000000040000);
   assert(ENCODE_NAME(PAN_ARCH)(&info, commands, sizeof(commands), &size));
   unsigned baseline_size = size;

   /* Optional probe writeback: a second SYNC_ADD64 must be appended before the
    * error barrier, and the stream must still be well formed. */
   info.probe_address = UINT64_C(0x500000);
   info.probe_value = UINT64_C(0x1122334455667788);
   assert(ENCODE_NAME(PAN_ARCH)(&info, commands, sizeof(commands), &size));
   assert(size > baseline_size);
   assert((commands[size / 8 - 1] >> 56) == MALI_CS_OPCODE_ERROR_BARRIER);

   unsigned add64_count = 0;
   for (unsigned i = 0; i < size / 8; i++) {
      if ((commands[i] >> 56) != MALI_CS_OPCODE_SYNC_ADD64)
         continue;
      struct MALI_CS_SYNC_ADD64 packet;
      MALI_CS_SYNC_ADD64_unpack(
         (const struct mali_cs_sync_add64_packed *)&commands[i], &packet);
      assert(packet.error_propagate && packet.scope == MALI_CS_SYNC_SCOPE_SYSTEM);
      add64_count++;
   }
   assert(add64_count == 2);

   /* With the probe disabled again the stream must return to its baseline. */
   info.probe_address = 0;
   info.probe_value = 0;
   assert(ENCODE_NAME(PAN_ARCH)(&info, commands, sizeof(commands), &size));
   assert(size == baseline_size);
   return 0;
}
