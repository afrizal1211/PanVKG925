#ifndef KBASE_KMOD_H
#define KBASE_KMOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pan_kmod.h"

extern const struct pan_kmod_ops kbase_kmod_ops;

bool kbase_kmod_parse_props(const uint8_t *buf, size_t size,
                            struct pan_kmod_dev_props *props);
bool kbase_kmod_is_csf(const struct pan_kmod_dev *dev);
bool kbase_kmod_supports_panvk(const struct pan_kmod_dev *dev);
struct pan_kmod_dev *kbase_kmod_dev_reopen(
   const struct pan_kmod_dev *physical, uint32_t flags,
   const struct pan_kmod_allocator *allocator);
uint64_t kbase_kmod_bo_gpu_va(struct pan_kmod_bo *bo);
int kbase_kmod_bo_sync(struct pan_kmod_bo *bo, uint64_t offset, uint64_t size,
                       enum pan_kmod_bo_sync_type type);

struct kbase_kmod_heap;
struct kbase_kmod_alias;

int kbase_kmod_jit_init(struct pan_kmod_dev *dev, uint64_t va_pages,
                        uint64_t phys_pages, uint32_t max_allocations);
struct kbase_kmod_heap *kbase_kmod_heap_create(
   struct pan_kmod_dev *dev, uint32_t chunk_size, uint32_t initial_chunks,
   uint32_t max_chunks, uint32_t target_in_flight, uint32_t memory_group);
int kbase_kmod_heap_destroy(struct kbase_kmod_heap *heap);
uint64_t kbase_kmod_heap_context(struct kbase_kmod_heap *heap);
uint64_t kbase_kmod_heap_first_chunk(struct kbase_kmod_heap *heap);
struct kbase_kmod_alias *kbase_kmod_alias_create(struct pan_kmod_bo *bo,
                                                uint64_t size, uint32_t count);
int kbase_kmod_alias_destroy(struct kbase_kmod_alias *alias);
uint64_t kbase_kmod_alias_gpu_va(struct kbase_kmod_alias *alias);
bool kbase_kmod_alias_cleanup_uncertain(const struct pan_kmod_dev *dev);

#endif
