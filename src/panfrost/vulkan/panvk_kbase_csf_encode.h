#ifndef PANVK_KBASE_CSF_ENCODE_H
#define PANVK_KBASE_CSF_ENCODE_H

#include <stdbool.h>
#include <stdint.h>

struct panvk_kbase_csf_encode_info {
   uint32_t register_count;
   uint32_t unpreserved_count;
   uint32_t scoreboard_count;
   uint32_t ls_slot;
   uint32_t flush_slot;
   uint32_t deferred_slot;
   uint32_t iterator_slot;
   uint32_t iterator_mask;
   uint32_t context_register;
   uint32_t resource_mask;
   uint64_t context_address;
   uint64_t heap_address;
   uint64_t stream_address;
   uint32_t stream_size;
   uint32_t flush_id;
   uint64_t seqno_address;
   /* Optional GPU writeback probe.  When probe_address is non-zero the stream
    * also performs a SYNC_ADD64 of probe_value at that address.  Used to make
    * the GPU prove it can reach a given address, in particular the aliased
    * ring window, which is not proven end to end yet.  Zero disables it and
    * leaves the production stream unchanged.
    */
   uint64_t probe_address;
   uint64_t probe_value;
};

typedef bool (*panvk_kbase_csf_encode_func)(
   const struct panvk_kbase_csf_encode_info *info,
   uint64_t *commands, uint32_t capacity_bytes, uint32_t *size_bytes);

bool panvk_v10_kbase_csf_encode(const struct panvk_kbase_csf_encode_info *,
                               uint64_t *, uint32_t, uint32_t *);
bool panvk_v11_kbase_csf_encode(const struct panvk_kbase_csf_encode_info *,
                               uint64_t *, uint32_t, uint32_t *);
bool panvk_v12_kbase_csf_encode(const struct panvk_kbase_csf_encode_info *,
                               uint64_t *, uint32_t, uint32_t *);
bool panvk_v13_kbase_csf_encode(const struct panvk_kbase_csf_encode_info *,
                               uint64_t *, uint32_t, uint32_t *);
bool panvk_v14_kbase_csf_encode(const struct panvk_kbase_csf_encode_info *,
                               uint64_t *, uint32_t, uint32_t *);

#endif
