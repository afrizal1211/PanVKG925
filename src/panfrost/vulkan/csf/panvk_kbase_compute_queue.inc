#if defined(HAVE_PAN_KMOD_KBASE)

#include <poll.h>
#include <string.h>

struct kbase_compute_entry {
   uint64_t commands[128];
   uint32_t size;
   uint32_t subqueue;
   uint64_t target;
   uint64_t stream;
   uint32_t stream_size;
   const uint64_t *stream_cpu;
};

static bool
kbase_compute_encode(struct panvk_gpu_queue *queue, uint32_t subqueue,
                       uint64_t stream, uint32_t stream_size, uint64_t heap,
                       struct kbase_compute_entry *entry)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const struct drm_panthor_csif_info *csif = panvk_get_csif_props(dev);
   struct panvk_kbase_csf_encode_info info = {
      .register_count = csif->cs_reg_count,
      .unpreserved_count = csif->unpreserved_cs_reg_count,
      .scoreboard_count = dev->csf.sb.count,
      .ls_slot = SB_ID(LS),
      .flush_slot = SB_ID(IMM_FLUSH),
      .deferred_slot = SB_ID(DEFERRED_SYNC),
      .iterator_slot = SB_ITER(0),
      .iterator_mask = dev->csf.sb.all_iters_mask,
      .context_register = PANVK_CS_REG_SUBQUEUE_CTX_START,
      .resource_mask = get_resource_mask(subqueue),
      .context_address = panvk_priv_mem_dev_addr(queue->subqueues[subqueue].context),
      .heap_address = subqueue == PANVK_SUBQUEUE_COMPUTE ? 0 : heap,
      .stream_address = stream,
      .stream_size = stream_size,
      .flush_id = panvk_get_flush_id(dev),
      .seqno_address = kbase_csf_queue_seqno_va(queue->kbase.queues[subqueue]),
   };
   /* Probe writeback: opt-in through PANVK_DEBUG=alias_writeback so the
    * production stream is byte-identical unless someone asks for it.
    * addr.dev is the aliased ring window base, so +RENDER_DESC_RINGBUF_SIZE
    * lands in the second alias entry, which is the extent no GPU access has
    * exercised yet.
    */
   if (PANVK_DEBUG(ALIAS_WRITEBACK) && queue->render_desc_ringbuf.addr.dev &&
       subqueue == PANVK_SUBQUEUE_COMPUTE) {
      info.probe_address = queue->render_desc_ringbuf.addr.dev +
                           RENDER_DESC_RINGBUF_SIZE;
      info.probe_value = 0x1122334455667788ull;
   }
   /* Geometry probe: same write, but aimed at the LAST alias entry, so the
    * entry walk is exercised for counts other than the production 2.  The
    * write lands at (entry_count-1)*stride into the repeated backing BO, which
    * for count N is offset ((N-1)*stride) mod stride == 0 in the backing page,
    * so the readback address stays the backing base.
    */
   if (PANVK_DEBUG(ALIAS_GEOMETRY) && queue->render_desc_ringbuf.addr.dev &&
       subqueue == PANVK_SUBQUEUE_COMPUTE) {
      const char *n = getenv("PANVK_KBASE_ALIAS_ENTRIES");
      long entries = (n && *n) ? strtol(n, NULL, 0) : 2;
      if (entries < 2 || entries > 4)
         entries = 2;
      info.probe_address = queue->render_desc_ringbuf.addr.dev +
                           (uint64_t)(entries - 1) * RENDER_DESC_RINGBUF_SIZE;
      info.probe_value = 0x1122334455667788ull;
   }
   entry->subqueue = subqueue;
   entry->stream = stream;
   entry->stream_size = stream_size;
   return panvk_per_arch(kbase_csf_encode)(&info, entry->commands,
                                           sizeof(entry->commands), &entry->size);
}

static VkResult
kbase_compute_execute(struct panvk_gpu_queue *queue,
                        const struct kbase_compute_entry *entries, uint32_t count,
                        bool wait_for_completion)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct kbase_csf_submit_entry submit[PANVK_SUBQUEUE_COUNT];
   struct kbase_csf_queue *queues[PANVK_SUBQUEUE_COUNT];
   uint64_t targets[PANVK_SUBQUEUE_COUNT];
   if (!count || count > PANVK_SUBQUEUE_COUNT)
      return VK_ERROR_UNKNOWN;
   for (uint32_t i = 0; i < count; i++) {
      uint32_t sq = entries[i].subqueue;
      queues[i] = queue->kbase.queues[sq];
      targets[i] = entries[i].target;
      submit[i] = (struct kbase_csf_submit_entry){
         .queue = queues[i], .commands = entries[i].commands,
         .size = entries[i].size, .target = targets[i],
      };
   }
   /* KCPU completion probe.
    *
    * The compute stream already ends with SYNC_ADD64 of +1 at the queue's
    * seqno address, and kbase_csf_wait() below polls that same word from the
    * CPU.  Parking a KCPU CQS wait on it asks the kernel's own event path the
    * same question, which is the mechanism a real completion signal needs.
    *
    * The wait and the fence signal go on a separate KCPU queue from the
    * submission: a CQS wait cannot be released by anything queued behind it.
    */
   struct kbase_csf_kcpu *kcpu_probe = NULL;
   int kcpu_probe_fd = -1;
   if (PANVK_DEBUG(KCPU_EVENT) && count == 1 &&
       entries[0].subqueue == PANVK_SUBQUEUE_COMPUTE) {
      uint64_t seqno_va = kbase_csf_queue_seqno_va(queues[0]);
      uint64_t expect = targets[0] - 1;
      kcpu_probe = kbase_csf_kcpu_create(dev->kbase.ctx);
      if (!kcpu_probe) {
         fprintf(stderr, "KBASE_KCPU_EVENT_RESULT stage=create_failed errno=%d\n",
                 errno);
      } else if (kbase_csf_kcpu_cqs_wait(kcpu_probe, seqno_va, expect)) {
         fprintf(stderr, "KBASE_KCPU_EVENT_RESULT stage=wait_failed errno=%d\n",
                 errno);
         kbase_csf_kcpu_destroy(kcpu_probe);
         kcpu_probe = NULL;
      } else if (kbase_csf_kcpu_fence_signal(kcpu_probe, &kcpu_probe_fd)) {
         fprintf(stderr, "KBASE_KCPU_EVENT_RESULT stage=signal_failed errno=%d\n",
                 errno);
         kbase_csf_kcpu_destroy(kcpu_probe);
         kcpu_probe = NULL;
      } else {
         fprintf(stderr, "KBASE_KCPU_EVENT_ARMED seqno=0x%016" PRIx64
                         " expect=%" PRIu64 " fd=%d\n",
                 seqno_va, expect, kcpu_probe_fd);
      }
   }

   while (kbase_csf_submit(submit, count)) {
      /* The userspace ring is smaller than the amount of work an
       * asynchronous submit can leave queued, so drain the queue's current
       * target and retry instead of failing the submission. */
      if (errno != EAGAIN) {
         if (kcpu_probe)
            kbase_csf_kcpu_destroy(kcpu_probe);
         return vk_queue_set_lost(&queue->vk, "kbase submission failed");
      }
      uint64_t pending[PANVK_SUBQUEUE_COUNT];
      bool any = false;
      for (uint32_t i = 0; i < count; i++) {
         pending[i] = queue->kbase.targets[entries[i].subqueue];
         any |= pending[i] != 0;
      }
      if (!any ||
          kbase_csf_wait(queues, count, pending,
                         os_time_get_nano() + 5000000000ull) != 1) {
         if (kcpu_probe)
            kbase_csf_kcpu_destroy(kcpu_probe);
         return vk_queue_set_lost(&queue->vk, "kbase ring reclaim failed");
      }
   }
   for (uint32_t i = 0; i < count; i++)
      queue->kbase.targets[entries[i].subqueue] = targets[i];
   if (!wait_for_completion)
      return VK_SUCCESS;
   uint64_t wait_ns = 5000000000ull;
   const char *wt = getenv("PANVK_KBASE_TEST_WAIT_NS");
   if (wt && *wt) {
      long long v = strtoll(wt, NULL, 0);
      if (v > 0)
         wait_ns = (uint64_t)v;
   }
   uint64_t deadline = os_time_get_nano() + wait_ns;
   int waited = kbase_csf_wait(queues, count, targets, deadline);
   if (waited != 1) {
      if (getenv("PANVK_KBASE_TEST_SYNCOBJS")) {
         /* Temporary diagnostic: the subqueue sync cells are written by the
          * GPU, so invalidate before reading.  Default off. */
         panvk_priv_mem_invalidate(
            queue->syncobjs, 0, 64);
         pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
         const uint8_t *cells = panvk_priv_mem_host_addr(queue->syncobjs);
         uint64_t wanted[PANVK_SUBQUEUE_COUNT] = {0};
         for (uint32_t i = 0; i < count; i++)
            wanted[entries[i].subqueue] = targets[i];
         if (!cells) {
            fprintf(stderr, "KBASE_TEST_SYNCOBJS cpu_ptr=null\n");
         } else {
            uint64_t cell[PANVK_SUBQUEUE_COUNT] = {0};
            for (unsigned k = 0; k < PANVK_SUBQUEUE_COUNT; k++)
               memcpy(&cell[k],
                      cells + k * sizeof(struct panvk_cs_sync64),
                      sizeof(uint64_t));
            fprintf(stderr,
                    "KBASE_TEST_SYNCOBJS cell0=%" PRIu64 " cell1=%" PRIu64
                    " cell2=%" PRIu64 " vt_target=%" PRIu64
                    " frag_target=%" PRIu64 " compute_target=%" PRIu64 "\n",
                    cell[0], cell[1], cell[2], wanted[0], wanted[1],
                    wanted[2]);
         }
         fflush(stderr);
      }
      if (getenv("PANVK_KBASE_TEST_WAIT_TRACE")) {
         int wait_errno = errno;
         fprintf(stderr, "KBASE_TEST_WAIT_RESULT ret=%d errno=%d (%s)\n",
                 waited, wait_errno, strerror(wait_errno));
         uint32_t subqueues = 0;
         uint64_t wanted[PANVK_SUBQUEUE_COUNT] = {0};
         for (uint32_t i = 0; i < count; i++) {
            subqueues |= BITFIELD_BIT(entries[i].subqueue);
            wanted[entries[i].subqueue] = targets[i];
         }
         fprintf(stderr,
                 "KBASE_TEST_WAIT first=%u count=%u subqueues=0x%x "
                 "vt_target=%" PRIu64 " frag_target=%" PRIu64
                 " vt_current=%" PRIu64 " frag_current=%" PRIu64 "\n",
                 entries[0].subqueue, count, subqueues, wanted[0], wanted[1],
                 queue->kbase.targets[0], queue->kbase.targets[1]);
         for (uint32_t i = 0; i < count; i++) {
            struct kbase_csf_queue *q = queues[i];
            const uint64_t *map = kbase_csf_queue_seqno_map(q);
            fprintf(stderr,
                    "KBASE_TEST_QUEUE idx=%u subqueue=%u target=%" PRIu64
                    " seqno_va=0x%" PRIx64 " published=%" PRIu64
                    " seqno_now=%" PRIu64 " err=%u"
                    " insert=%" PRIu64 " extract=%" PRIu64 " active=%u\n",
                    i, entries[i].subqueue, targets[i],
                    kbase_csf_queue_seqno_va(q),
                    kbase_csf_queue_published_target(q),
                    map ? map[0] : 0,
                    map ? *(uint32_t *)((uint8_t *)map + 8) : 0,
                    kbase_csf_queue_published_insert(q),
                    kbase_csf_queue_extract(q), kbase_csf_queue_active(q));
            if (entries[i].stream_cpu && entries[i].stream_size) {
               uint64_t words = entries[i].stream_size / 8;
               if (words > 96)
                  words = 96;
               fprintf(stderr, "KBASE_TEST_STREAM idx=%u words=%" PRIu64 "\n",
                       i, words);
               for (uint64_t w = 0; w < words; w += 8) {
                  fprintf(stderr, "  +%03" PRIu64 ":", w * 8);
                  for (uint64_t k = 0; k < 8 && w + k < words; k++)
                     fprintf(stderr, " %016" PRIx64, entries[i].stream_cpu[w + k]);
                  fprintf(stderr, "\n");
               }
            }
            uint64_t ring_size = 0;
            const uint8_t *ring = kbase_csf_queue_ring_map(q, &ring_size);
            if (ring && ring_size) {
               uint64_t at = kbase_csf_queue_extract(q);
               for (uint32_t c = 0; c < 5; c++) {
                  uint64_t line = (at - 128 + c * 64) & (ring_size - 1);
                  fprintf(stderr, "KBASE_TEST_RING idx=%u off=%" PRIu64
                                  " (extract=%" PRIu64 "):", i, line, at);
                  for (uint32_t w = 0; w < 8; w++) {
                     uint64_t off = (line + w * 8) & (ring_size - 1);
                     uint64_t val;
                     memcpy(&val, ring + off, sizeof(val));
                     fprintf(stderr, " %016" PRIx64, val);
                  }
                  fprintf(stderr, "\n");
               }
            }
         }
         fflush(stderr);
      }
      if (kcpu_probe)
         kbase_csf_kcpu_destroy(kcpu_probe);
      return vk_queue_set_lost(&queue->vk, "kbase completion failed");
   }

   if (kcpu_probe) {
      struct pollfd pfd = {.fd = kcpu_probe_fd, .events = POLLIN};
      int ready = poll(&pfd, 1, 2000);
      fprintf(stderr, "KBASE_KCPU_EVENT_RESULT ready=%d revents=0x%x "
                      "target=%" PRIu64 "\n",
              ready, ready > 0 ? pfd.revents : 0, targets[0]);
      close(kcpu_probe_fd);
      kcpu_probe_fd = -1;
      kbase_csf_kcpu_destroy(kcpu_probe);
   }

   /* Probe readback: with PANVK_DEBUG=alias_writeback the compute stream also
    * wrote a known value into the second alias entry.  The alias repeats one
    * backing BO, so that write lands at the start of the backing mapping, and
    * addr.host points exactly there.  A match proves the GPU resolved the
    * second entry to the expected physical page.
    *
    * The same execute path also runs the queue's init stream, which carries no
    * probe write, so the first observation legitimately reads zero.  Report
    * each one and let the last submission be the one that must match.
    */
   if ((PANVK_DEBUG(ALIAS_WRITEBACK) || PANVK_DEBUG(ALIAS_GEOMETRY)) &&
       queue->render_desc_ringbuf.addr.host) {
      const uint64_t expected = 0x1122334455667788ull;
      const uint64_t observed =
         *(volatile uint64_t *)queue->render_desc_ringbuf.addr.host;
      fprintf(stderr, "KBASE_ALIAS_WRITEBACK_RESULT expected=0x%016" PRIx64
                      " observed=0x%016" PRIx64 " match=%u\n",
              expected, observed, observed == expected);
      fflush(stderr);
   }
   return VK_SUCCESS;
}

static bool
kbase_compute_cleanup(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   bool stopped = true;
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (queue->kbase.queues[i] && kbase_csf_queue_stop(queue->kbase.queues[i]))
         stopped = false;
   }
   if (!stopped) {
      dev->kbase.quarantined = true;
      return false;
   }
   if (queue->kbase.retired_heap && kbase_kmod_heap_destroy(queue->kbase.retired_heap)) {
      dev->kbase.quarantined = true;
      return false;
   }
   queue->kbase.retired_heap = NULL;
   if (queue->kbase.heap && kbase_kmod_heap_destroy(queue->kbase.heap)) {
      dev->kbase.quarantined = true;
      return false;
   }
   queue->kbase.heap = NULL;
   if (queue->kbase.desc_alias && kbase_kmod_alias_destroy(queue->kbase.desc_alias)) {
      dev->kbase.quarantined = true;
      return false;
   }
   queue->kbase.desc_alias = NULL;
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      if (queue->kbase.queues[i]) {
         kbase_csf_queue_unref(queue->kbase.queues[i]);
         queue->kbase.queues[i] = NULL;
      }
      panvk_pool_free_mem(&queue->subqueues[i].context);
      panvk_pool_free_mem(&queue->subqueues[i].regs_save);
   }
   panvk_pool_free_mem(&queue->syncobjs);
   panvk_pool_free_mem(&queue->render_desc_ringbuf.syncobj);
   if (queue->render_desc_ringbuf.bo) {
      pan_kmod_bo_put(queue->render_desc_ringbuf.bo);
      queue->render_desc_ringbuf.bo = NULL;
   }
   /* addr.host is a borrowed view of the backing mapping, not an owned one:
    * bo_free() and kbase_kmod_alias_destroy() already released it.  Clear it
    * so nothing downstream tries to unmap through it.
    */
   queue->render_desc_ringbuf.addr.host = NULL;
   panvk_pool_free_mem(&queue->tiler_heap.desc);
   panvk_pool_free_mem(&queue->tiler_heap.oom_fbd);
   if (queue->kbase.init_stream) {
      panvk_priv_bo_unref(queue->kbase.init_stream);
      queue->kbase.init_stream = NULL;
   }
   return true;
}

static struct kbase_kmod_heap *
kbase_graphics_heap_create(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   return kbase_kmod_heap_create(dev->kmod.dev, 1024 * 1024, 10, 400,
                                 UINT16_MAX, 0);
}

static void
kbase_graphics_heap_desc(struct panvk_gpu_queue *queue, struct kbase_kmod_heap *heap)
{
   queue->tiler_heap.context.dev_addr = kbase_kmod_heap_context(heap);
   queue->tiler_heap.chunk_size = 1024 * 1024;
   panvk_priv_mem_write_desc(queue->tiler_heap.desc, 0, TILER_HEAP, cfg) {
      cfg.size = queue->tiler_heap.chunk_size;
      cfg.base = kbase_kmod_heap_first_chunk(heap);
      cfg.bottom = cfg.base + 64;
      cfg.top = cfg.base + cfg.size;
   }
}

static VkResult
kbase_graphics_resources(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   if (kbase_kmod_jit_init(dev->kmod.dev, 1ull << 25, 1ull << 25, 255))
      return VK_ERROR_INITIALIZATION_FAILED;
   queue->kbase.heap = kbase_graphics_heap_create(queue);
   if (!queue->kbase.heap)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct panvk_pool_alloc_info alloc = {.size = 4096 + 65536, .alignment = 4096};
   queue->tiler_heap.desc = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
   if (!panvk_priv_mem_check_alloc(queue->tiler_heap.desc))
      return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
   kbase_graphics_heap_desc(queue, queue->kbase.heap);
   struct panvk_desc_ringbuf *ring = &queue->render_desc_ringbuf;
   ring->size = RENDER_DESC_RINGBUF_SIZE;
   ring->bo = pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, ring->size, 0);
   if (!ring->bo)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   /* Entry count is fixed at 2 in production (a double-buffered window).  The
    * alias geometry probe overrides it to check that GPU resolution of the
    * later entries holds for other counts; see PANVK_DEBUG=alias_geometry.
    */
   uint32_t alias_entries = 2;
   if (PANVK_DEBUG(ALIAS_GEOMETRY)) {
      const char *n = getenv("PANVK_KBASE_ALIAS_ENTRIES");
      if (n && *n) {
         long v = strtol(n, NULL, 0);
         if (v >= 2 && v <= 4)
            alias_entries = (uint32_t)v;
      }
   }
   queue->kbase.desc_alias =
      kbase_kmod_alias_create(ring->bo, ring->size, alias_entries);
   if (!queue->kbase.desc_alias)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   ring->addr.dev = kbase_kmod_alias_gpu_va(queue->kbase.desc_alias);
   /* SAME_VA makes the backing BO's CPU mapping address equal to its GPU
    * address.  Record it so the window can be inspected from the driver, and
    * so a debug probe has something to compare against.
    *
    * Read-only by contract: the backing mapping is owned by bo_free and the
    * alias mapping is owned by kbase_kmod_alias_destroy(), both of which
    * munmap it themselves.  kbase_compute_cleanup() therefore clears this
    * field instead of unmapping through it, which would double-munmap.
    */
   ring->addr.host = kbase_kmod_bo_gpu_va(ring->bo) ?
                        (void *)(uintptr_t)kbase_kmod_bo_gpu_va(ring->bo) :
                        NULL;
   alloc = (struct panvk_pool_alloc_info){.size = sizeof(struct panvk_cs_sync32),
                                          .alignment = 64};
   ring->syncobj = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
   if (!panvk_priv_mem_check_alloc(ring->syncobj))
      return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
   panvk_priv_mem_write(ring->syncobj, 0, struct panvk_cs_sync32, syncobj) {
      *syncobj = (struct panvk_cs_sync32){.seqno = RENDER_DESC_RINGBUF_SIZE};
   }
   queue->kbase.graphics_ready = true;
   return VK_SUCCESS;
}

static VkResult
kbase_compute_init(struct panvk_gpu_queue *queue)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   if (PANVK_DEBUG(TRACE))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   if (kbase_kmod_jit_init(dev->kmod.dev, 1ull << 25, 1ull << 25, 255))
      return VK_ERROR_INITIALIZATION_FAILED;
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      queue->kbase.queues[i] = kbase_csf_queue_create(dev->kbase.ctx, 65536, 1);
      if (!queue->kbase.queues[i])
         return VK_ERROR_INITIALIZATION_FAILED;
   }
   VkResult result = kbase_graphics_resources(queue);
   if (result != VK_SUCCESS)
      return result;
   struct panvk_pool_alloc_info alloc = {
      .size = ALIGN_POT(sizeof(struct panvk_cs_sync64), 64) * PANVK_SUBQUEUE_COUNT,
      .alignment = 64,
   };
   queue->syncobjs = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
   if (!panvk_priv_mem_check_alloc(queue->syncobjs))
      return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      panvk_priv_mem_write(queue->syncobjs, i * sizeof(struct panvk_cs_sync64),
                           struct panvk_cs_sync64, cell) {
         *cell = (struct panvk_cs_sync64){.seqno = 16};
      }
   }
   result = panvk_priv_bo_create(dev, 4096 * (PANVK_SUBQUEUE_COUNT + 1),
                                 PAN_KMOD_BO_FLAG_WB_MMAP,
                                 VK_SYSTEM_ALLOCATION_SCOPE_DEVICE,
                                 &queue->kbase.init_stream);
   if (result != VK_SUCCESS)
      return result;
   const struct drm_panthor_csif_info *csif = panvk_get_csif_props(dev);
   struct kbase_compute_entry init_entries[PANVK_SUBQUEUE_COUNT];
   for (unsigned i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct panvk_subqueue *subq = &queue->subqueues[i];
      if (dev->dump_region_size[i]) {
         alloc.size = dev->dump_region_size[i];
         subq->regs_save = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
         if (!panvk_priv_mem_check_alloc(subq->regs_save))
            return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
      alloc.size = sizeof(struct panvk_cs_subqueue_context);
      subq->context = panvk_pool_alloc_mem(&dev->mempools.rw, alloc);
      if (!panvk_priv_mem_check_alloc(subq->context))
         return panvk_catch_indirect_alloc_failure(VK_ERROR_OUT_OF_DEVICE_MEMORY);
      panvk_priv_mem_write(subq->context, 0, struct panvk_cs_subqueue_context, ctx) {
         *ctx = (struct panvk_cs_subqueue_context){
            .syncobjs = panvk_priv_mem_dev_addr(queue->syncobjs),
            .reg_dump_addr = panvk_priv_mem_dev_addr(subq->regs_save),
#if PAN_ARCH == 10
            .iter_sb = SB_ITER(0),
#endif
         };
         if (i != PANVK_SUBQUEUE_COMPUTE) {
            ctx->render.tiler_heap = panvk_priv_mem_dev_addr(queue->tiler_heap.desc);
            ctx->render.geom_buf = (ctx->render.tiler_heap + 4096) | 16;
            ctx->render.desc_ringbuf = (struct panvk_cs_desc_ringbuf){
               .syncobj = panvk_priv_mem_dev_addr(queue->render_desc_ringbuf.syncobj),
               .ptr = queue->render_desc_ringbuf.addr.dev,
            };
         }
      }
      struct cs_builder_conf conf = {.nr_registers = csif->cs_reg_count,
                                     .nr_kernel_registers = csif->unpreserved_cs_reg_count,
                                     .ls_sb_slot = SB_ID(LS)};
      struct cs_buffer buffer = {
         .cpu = (uint64_t *)((uint8_t *)queue->kbase.init_stream->addr.host + i * 4096),
         .gpu = queue->kbase.init_stream->addr.dev + i * 4096,
         .capacity = 4096 / sizeof(uint64_t),
      };
      struct cs_builder b;
      cs_builder_init(&b, &conf, buffer);
      cs_move64_to(&b, cs_subqueue_ctx_reg(&b), panvk_priv_mem_dev_addr(subq->context));
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++)
         cs_move64_to(&b, cs_progress_seqno_reg(&b, j), 0);
      cs_end(&b);
      queue->kbase.init_size[i] = cs_is_valid(&b) ? cs_root_chunk_size(&b) : 0;
      cs_builder_fini(&b);
      if (!queue->kbase.init_size[i])
         return VK_ERROR_INITIALIZATION_FAILED;
      panvk_priv_bo_flush(queue->kbase.init_stream, i * 4096, queue->kbase.init_size[i]);
      init_entries[i].target = 1;
      if (!kbase_compute_encode(queue, i, buffer.gpu, queue->kbase.init_size[i],
                                 queue->tiler_heap.context.dev_addr, &init_entries[i]))
         return VK_ERROR_INITIALIZATION_FAILED;
   }
   memset((uint8_t *)queue->kbase.init_stream->addr.host +
             PANVK_SUBQUEUE_COUNT * 4096, 0, sizeof(uint64_t));
   panvk_priv_bo_flush(queue->kbase.init_stream, PANVK_SUBQUEUE_COUNT * 4096,
                        sizeof(uint64_t));
   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
   return kbase_compute_execute(queue, init_entries, PANVK_SUBQUEUE_COUNT,
                                true);
}

/* First-frame readiness report.
 *
 * Static scaffolding only: it describes what a graphics submission would do
 * and refuses nothing.  It exists so the first-frame experiment has a single
 * place to assert its preconditions, and so the assumptions written down in
 * first-frame-prep-20261018/EXPERIMENT-CARD.md have a counterpart in code.
 *
 * Deliberately does not submit, encode, or touch the heap.  Enabling it
 * changes no behaviour beyond a debug line.
 */
struct kbase_first_frame_report {
   bool graphics_ready;
   bool has_graphics_chunk;
   bool has_tiler_heap;
   bool desc_alias_present;
   unsigned graphics_subqueues;
   uint64_t tiler_heap_context;
};

static struct kbase_first_frame_report
kbase_first_frame_probe(struct panvk_gpu_queue *queue,
                        const struct vk_queue_submit *submit)
{
   struct kbase_first_frame_report report = {
      .graphics_ready = queue->kbase.graphics_ready,
      .has_tiler_heap = queue->kbase.heap != NULL,
      .desc_alias_present = queue->kbase.desc_alias != NULL,
      .tiler_heap_context = queue->tiler_heap.context.dev_addr,
   };
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      struct panvk_cmd_buffer *cmdbuf =
         container_of(submit->command_buffers[i], struct panvk_cmd_buffer, vk);
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++) {
         struct cs_builder *b = panvk_get_cs_builder(cmdbuf, j);
         if (!cs_root_chunk_size(b))
            continue;
         if (j != PANVK_SUBQUEUE_COMPUTE) {
            report.has_graphics_chunk = true;
            report.graphics_subqueues |= 1u << j;
         }
      }
   }
   return report;
}

/* Completion object for an asynchronous submission: the signal is not
 * satisfied by the CPU, only by the GPU seqnos the submit published. */
static VkResult
kbase_completion_wait(void *data, uint32_t target_count,
                      const uint64_t *targets, uint64_t abs_timeout_ns)
{
   struct panvk_gpu_queue *queue = data;
   struct kbase_csf_queue *queues[PANVK_SUBQUEUE_COUNT];
   if (target_count > PANVK_SUBQUEUE_COUNT)
      return VK_ERROR_UNKNOWN;
   for (uint32_t i = 0; i < target_count; i++)
      queues[i] = queue->kbase.queues[i];
   int waited = kbase_csf_wait(queues, target_count, targets, abs_timeout_ns);
   if (waited == 1)
      return VK_SUCCESS;
   /* The sync layer hands out short absolute slices and retries, so an
    * expired slice is a timeout rather than a lost device. */
   if (waited == 0)
      return VK_TIMEOUT;
   return vk_queue_set_lost(&queue->vk, "kbase completion failed");
}

static void
kbase_completion_retain(void *data)
{
}

static void
kbase_completion_release(void *data)
{
}

static const struct panvk_kbase_completion_ops kbase_completion_ops = {
   .wait = kbase_completion_wait,
   .retain = kbase_completion_retain,
   .release = kbase_completion_release,
};

static VkResult
kbase_compute_submit(struct panvk_gpu_queue *queue, struct vk_queue_submit *submit)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   const char *async_env = getenv("PANVK_KBASE_ASYNC_SUBMIT");
   const bool async_submit = async_env == NULL || strcmp(async_env, "0") != 0;
   if (PANVK_DEBUG(FIRST_FRAME)) {
      struct kbase_first_frame_report r = kbase_first_frame_probe(queue, submit);
      fprintf(stderr,
              "KBASE_FIRST_FRAME graphics_ready=%u graphics_chunk=%u "
              "graphics_subqueues=0x%x tiler_heap=%u heap_ctx=0x%llx "
              "desc_alias=%u\n",
              r.graphics_ready, r.has_graphics_chunk, r.graphics_subqueues,
              r.has_tiler_heap, (unsigned long long)r.tiler_heap_context,
              r.desc_alias_present);
      fflush(stderr);
   }
   if (vk_queue_submit_has_bind(submit) || PANVK_DEBUG(TRACE))
      return VK_ERROR_FEATURE_NOT_PRESENT;
   for (uint32_t i = 0; i < submit->wait_count; i++) {
      if (submit->waits[i].sync->type != &panvk_kbase_sync_type || submit->waits[i].wait_value)
         return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   for (uint32_t i = 0; i < submit->signal_count; i++) {
      if (submit->signals[i].sync->type != &panvk_kbase_sync_type || submit->signals[i].signal_value)
         return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   if (submit->command_buffer_count > SIZE_MAX / (PANVK_SUBQUEUE_COUNT * sizeof(struct kbase_compute_entry)))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   bool graphics = false;
   uint64_t next_targets[PANVK_SUBQUEUE_COUNT];
   memcpy(next_targets, queue->kbase.targets, sizeof(next_targets));
   for (uint32_t i = 0; i < submit->command_buffer_count; i++) {
      struct panvk_cmd_buffer *cmdbuf = container_of(submit->command_buffers[i], struct panvk_cmd_buffer, vk);
      if (cmdbuf->state.contains_event_waits || cmdbuf->state.contains_timestamp_queries ||
          cmdbuf->state.uses_poly_heap)
         return VK_ERROR_FEATURE_NOT_PRESENT;
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++) {
         struct cs_builder *b = panvk_get_cs_builder(cmdbuf, j);
         if (!cs_is_valid(b))
            return VK_ERROR_UNKNOWN;
         if (!cs_root_chunk_size(b))
            continue;
         if (next_targets[j] == UINT64_MAX)
            return VK_ERROR_UNKNOWN;
         next_targets[j]++;
         graphics |= j != PANVK_SUBQUEUE_COMPUTE;
      }
   }
   if (graphics && !queue->kbase.graphics_ready)
      return VK_ERROR_FEATURE_NOT_PRESENT;
   /* The fragment stream carries no FRAGMENT_COMPLETED heap op on kbase, so
    * the heap only grows between renewals.  Renewing on every frame keeps
    * the accounting trivial, but the replacement races the previous heap
    * generation still in flight often enough to strand the fragment seqno;
    * amortize renewal over several submissions the way the reference
    * implementation does. */
   bool renew_heap = false;
   if (graphics) {
      static uint32_t renew_interval;
      if (!renew_interval) {
         const char *iv = getenv("PANVK_KBASE_TEST_HEAP_INTERVAL");
         long v = (iv && *iv) ? strtol(iv, NULL, 0) : 128;
         renew_interval = (v > 0 && v <= UINT32_MAX) ? (uint32_t)v : 1;
      }
      queue->kbase.tiler_submit_count++;
      renew_heap = queue->kbase.tiler_submit_count >= renew_interval;
   }
   struct kbase_kmod_heap *replacement = NULL;
   if (renew_heap) {
      replacement = kbase_graphics_heap_create(queue);
      if (!replacement) {
         if (PANVK_DEBUG(FIRST_FRAME))
            fprintf(stderr, "KBASE_FIRST_FRAME_HEAP_FAIL errno=%d (%s)\n",
                    errno, strerror(errno));
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
      queue->kbase.tiler_submit_count = 0;
   }
   uint32_t wave_count = submit->command_buffer_count;
   struct kbase_compute_entry *entries = wave_count ?
      calloc((size_t)wave_count * PANVK_SUBQUEUE_COUNT, sizeof(*entries)) : NULL;
   if (wave_count && !entries) {
      if (replacement && kbase_kmod_heap_destroy(replacement)) {
         queue->kbase.retired_heap = replacement;
         dev->kbase.quarantined = true;
      }
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   struct kbase_compute_entry rearm[2];
   uint64_t planned[PANVK_SUBQUEUE_COUNT];
   memcpy(planned, queue->kbase.targets, sizeof(planned));
   uint64_t heap = renew_heap ? kbase_kmod_heap_context(replacement) :
                                kbase_kmod_heap_context(queue->kbase.heap);
   VkResult result = VK_SUCCESS;
   if (renew_heap) {
      for (unsigned j = 0; j < 2; j++) {
         if (planned[j] == UINT64_MAX) {
            result = VK_ERROR_UNKNOWN;
            goto out;
         }
         rearm[j].target = ++planned[j];
         uint64_t noop_stream = queue->kbase.init_stream->addr.dev +
                                PANVK_SUBQUEUE_COUNT * 4096;
         if (!kbase_compute_encode(queue, j, noop_stream, sizeof(uint64_t), heap, &rearm[j])) {
            result = VK_ERROR_UNKNOWN;
            goto out;
         }
      }
   }
   for (uint32_t i = 0; i < wave_count; i++) {
      struct panvk_cmd_buffer *cmdbuf = container_of(submit->command_buffers[i], struct panvk_cmd_buffer, vk);
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++) {
         struct cs_builder *b = panvk_get_cs_builder(cmdbuf, j);
         if (!cs_root_chunk_size(b))
            continue;
         struct kbase_compute_entry *entry = &entries[i * PANVK_SUBQUEUE_COUNT + j];
         if (planned[j] == UINT64_MAX) {
            result = VK_ERROR_UNKNOWN;
            goto out;
         }
         entry->target = ++planned[j];
         if (!kbase_compute_encode(queue, j, cs_root_chunk_gpu_addr(b), cs_root_chunk_size(b), heap, entry)) {
            result = VK_ERROR_UNKNOWN;
            goto out;
         }
         entry->stream_cpu = b->root_chunk.buffer.cpu;
      }
   }
   result = vk_sync_wait_many(&dev->vk, submit->wait_count, submit->waits,
                              VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
   if (result != VK_SUCCESS)
      goto out;
   for (uint32_t i = 0; i < submit->wait_count; i++) {
      result = vk_sync_reset(&dev->vk, submit->waits[i].sync);
      if (result != VK_SUCCESS) {
         result = vk_queue_set_lost(&queue->vk, "kbase wait consumption failed");
         goto out;
      }
   }
   if (renew_heap) {
      struct kbase_kmod_heap *old = queue->kbase.heap;
      queue->kbase.retired_heap = old;
      queue->kbase.heap = replacement;
      replacement = NULL;
      kbase_graphics_heap_desc(queue, queue->kbase.heap);
      pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
      result = kbase_compute_execute(queue, rearm, 2, true);
      if (result != VK_SUCCESS)
         goto out;
      if (kbase_kmod_heap_destroy(old)) {
         dev->kbase.quarantined = true;
         result = vk_queue_set_lost(&queue->vk, "kbase old heap retirement failed");
         goto out;
      }
      queue->kbase.retired_heap = NULL;
   }
   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
   for (uint32_t i = 0; i < wave_count; i++) {
      struct kbase_compute_entry wave[PANVK_SUBQUEUE_COUNT];
      uint32_t count = 0;
      for (unsigned j = 0; j < PANVK_SUBQUEUE_COUNT; j++) {
         struct kbase_compute_entry *entry = &entries[i * PANVK_SUBQUEUE_COUNT + j];
         if (entry->size)
            wave[count++] = *entry;
      }
      if (!count)
         continue;
      result = kbase_compute_execute(queue, wave, count, !async_submit);
      if (result != VK_SUCCESS)
         goto out;
   }
   for (uint32_t i = 0; i < submit->signal_count; i++) {
      if (async_submit) {
         result = panvk_kbase_sync_set_pending(
            submit->signals[i].sync, &kbase_completion_ops, queue,
            PANVK_SUBQUEUE_COUNT, queue->kbase.targets);
      } else {
         result = vk_sync_signal(&dev->vk, submit->signals[i].sync, 0);
      }
      if (result != VK_SUCCESS) {
         result = vk_queue_set_lost(&queue->vk, "kbase signal failed");
         break;
      }
   }
out:
   free(entries);
   if (replacement && kbase_kmod_heap_destroy(replacement)) {
      queue->kbase.retired_heap = replacement;
      dev->kbase.quarantined = true;
   }
   return result;
}
#endif
