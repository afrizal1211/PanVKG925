#define DEVICE_COOKIE_MAX_SIZE (4u * 1024u * 1024u)
#define DEVICE_QUEUE_RING_SIZE 65536u
#define DEVICE_MAP_LEDGER_MAX 512u

struct device_map_entry {
   void *address;
   size_t size;
   bool live;
};

enum device_request {
   DEV_VERSION_CHECK,
   DEV_SET_FLAGS,
   DEV_GPUPROPS,
   DEV_CSIF,
   DEV_JIT,
   DEV_ALLOC_EX,
   DEV_FREE,
   DEV_SYNC,
   DEV_ALIAS,
   DEV_QUEUE_REGISTER,
   DEV_GROUP_CREATE,
   DEV_QUEUE_BIND,
   DEV_QUEUE_KICK,
   DEV_QUEUE_TERM,
   DEV_GROUP_TERM,
   DEV_HEAP_INIT,
   DEV_HEAP_TERM,
   DEV_REQUEST_COUNT,
};

static const char *const device_request_names[DEV_REQUEST_COUNT] = {
   "version", "set_flags", "gpuprops", "csif", "jit", "alloc_ex", "free", "sync", "alias",
   "q_register", "q_group_create", "q_bind", "q_kick", "q_terminate", "q_group_terminate",
   "heap_init", "heap_term",
};

static struct {
   unsigned requests[DEV_REQUEST_COUNT];
   unsigned opens, closes, maps, unmaps, reads, polls;
   unsigned alloc_pages_max;
   unsigned group_handles_valid;
   bool created, queue_returned, destroyed;
   bool physical_phase_done;
   bool saw_same_va, saw_csf_event, saw_gpu_ex;
   bool cleanup_uncertain;
   bool submit_attempted, submit_accepted, submit_drained;
   unsigned submit_kicks_before, submit_polls_before, submit_reads_before;
   int unused_fd;
   struct device_map_entry maps_ledger[DEVICE_MAP_LEDGER_MAX];
   unsigned map_ledger_count;
   unsigned map_ledger_overflow;
   unsigned live_maps;
   const char *reason;
} device_probe;

static bool
device_is_logical_fd(int fd)
{
   return probe.reopen_fd > 0 && fd == probe.reopen_fd;
}

static void
device_note(enum device_request request)
{
   device_probe.requests[request]++;
}

static bool
device_mmap_allowed(int fd, size_t size, int prot, int flags, off_t offset, const char **kind)
{
   if ((fd != probe.fd && !device_is_logical_fd(fd)) || flags != MAP_SHARED || !size ||
       (size & 4095))
      return false;
   if (size == 4096 && prot == PROT_NONE && offset == (off_t)BASE_MEM_MAP_TRACKING_HANDLE) {
      *kind = "tracking";
      return true;
   }
   if (size == 4096 && prot == PROT_READ &&
       offset == (off_t)BASEP_MEM_CSF_USER_REG_PAGE_HANDLE) {
      *kind = "user_reg";
      return true;
   }
   if (size == (size_t)BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096 &&
       prot == (PROT_READ | PROT_WRITE) &&
       offset == (off_t)BASEP_MEM_CSF_USER_IO_PAGES_HANDLE) {
      *kind = "user_io_legacy";
      return true;
   }
   if (size <= DEVICE_COOKIE_MAX_SIZE && (prot == PROT_READ || prot == (PROT_READ | PROT_WRITE)) &&
       offset >= (off_t)BASE_MEM_COOKIE_BASE && offset < (off_t)BASE_MEM_FIRST_FREE_ADDRESS &&
       !(offset & 4095)) {
      *kind = "cookie";
      return true;
   }
   return false;
}

static int
device_ioctl(int fd, unsigned long request, void *arg)
{
   bool logical = device_is_logical_fd(fd);
   if (request == KBASE_IOCTL_VERSION_CHECK_CSF) {
      struct kbase_ioctl_version_check *r = arg;
      if (r->major || r->minor || probe.version_calls >= 2)
         return blocked();
      probe.version_calls++;
      int result = probe.live ? host_ioctl(fd, request, r) : 0;
      if (!probe.live) {
         /* kbase_kmod_dev_create rejects any CSF major != 1, so the CPU path
          * must report the same shape the live kernel reported for M5a. */
         r->major = 1;
         r->minor = 30;
      }
      probe.major = r->major;
      probe.minor = r->minor;
      device_note(DEV_VERSION_CHECK);
      printf("DEVICE_VERSION fd=%s ret=%d version=%u.%u\n", logical ? "logical" : "physical", result,
             r->major, r->minor);
      return result;
   }
   if (request == KBASE_IOCTL_SET_FLAGS) {
      const struct kbase_ioctl_set_flags *r = arg;
      if (r->create_flags || probe.flags_calls >= 2)
         return blocked();
      probe.flags_calls++;
      device_note(DEV_SET_FLAGS);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      printf("DEVICE_SET_FLAGS fd=%s ret=%d flags=0\n", logical ? "logical" : "physical", result);
      return result;
   }
   if (request == KBASE_IOCTL_GET_GPUPROPS) {
      struct kbase_ioctl_get_gpuprops *r = arg;
      bool query = r->buffer == 0;
      if (probe.props_calls >= 4 || r->flags)
         return blocked();
      if (query && r->size)
         return blocked();
      if (!query && (!r->size || r->size > PROPS_LIMIT))
         return blocked();
      probe.props_calls++;
      device_note(DEV_GPUPROPS);
      int result;
      if (probe.live)
         result = host_ioctl(fd, request, arg);
      else if (query)
         result = synthetic_properties(probe.properties);
      else {
         memcpy((void *)(uintptr_t)r->buffer, probe.properties, probe.size);
         result = probe.size;
      }
      if (query) {
         if (result > 0 && result <= PROPS_LIMIT)
            probe.size = result;
      } else if (result >= 0 && result <= PROPS_LIMIT) {
         unsigned actual = result ? result : probe.size;
         memcpy(probe.properties, (void *)(uintptr_t)r->buffer, actual);
         probe.size = actual;
      }
      printf("DEVICE_GPUPROPS fd=%s phase=%s ret=%d size=%u errno=%d\n",
             logical ? "logical" : "physical", query ? "query" : "fill", result, probe.size,
             result < 0 ? errno : 0);
      return result;
   }
   /* Every request below belongs to the logical device: it must already exist. */
   if (!logical)
      return blocked();
   if (request == KBASE_IOCTL_CS_GET_GLB_IFACE) {
      union kbase_ioctl_cs_get_glb_iface *r = arg;
      device_note(DEV_CSIF);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      if (probe.live)
         return result;
      /* kbase_csf_context_create rejects a zero group/stream count with
       * EPROTO, so the CPU path must report the same interface shape the
       * kernel and the icd_test mock report. */
      const uint32_t groups = 4, streams = 12;
      if (r->in.max_group_num && (r->in.max_group_num != groups ||
                                  r->in.max_total_stream_num != streams))
         return blocked();
      if (r->in.max_group_num) {
         struct basep_cs_group_control *g = (void *)(uintptr_t)r->in.groups_ptr;
         struct basep_cs_stream_control *s = (void *)(uintptr_t)r->in.streams_ptr;
         if (!g || !s)
            return blocked();
         for (uint32_t i = 0; i < groups; i++)
            g[i] = (struct basep_cs_group_control){.stream_num = 3};
         for (uint32_t i = 0; i < streams; i++)
            s[i] = (struct basep_cs_stream_control){.features = 8 << 8};
      }
      memset(r, 0, sizeof(*r));
      r->out.group_num = groups;
      r->out.total_stream_num = streams;
      r->out.glb_version = 0x1001e;
      device_note(DEV_CSIF);
      printf("DEVICE_CSIF groups=%u streams=%u version=0x%x\n", groups, streams, r->out.glb_version);
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_JIT_INIT) {
      const struct kbase_ioctl_mem_jit_init *r = arg;
      if (!r->va_pages || !r->phys_pages || r->phys_pages > r->va_pages || !r->max_allocations)
         return blocked();
      device_note(DEV_JIT);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      printf("DEVICE_JIT va_pages=%llu phys_pages=%llu max_allocations=%u ret=%d\n",
             (unsigned long long)r->va_pages, (unsigned long long)r->phys_pages,
             r->max_allocations, result);
      return result;
   }
   if (request == KBASE_IOCTL_MEM_ALLOC_EX) {
      union kbase_ioctl_mem_alloc_ex *r = arg;
      uint64_t forbidden = BASE_MEM_PROTECTED | BASE_MEM_IMPORT_SHARED | BASE_MEM_GROW_ON_GPF |
                           BASE_MEM_IMPORT_SYNC_ON_MAP_UNMAP;
      if (!r->in.va_pages || r->in.commit_pages != r->in.va_pages || (r->in.flags & forbidden) ||
          !(r->in.flags & BASE_MEM_SAME_VA))
         return blocked();
      device_probe.saw_same_va = true;
      device_probe.saw_csf_event |= (r->in.flags & BASE_MEM_CSF_EVENT) != 0;
      device_probe.saw_gpu_ex |= (r->in.flags & BASE_MEM_PROT_GPU_EX) != 0;
      if (r->in.va_pages > device_probe.alloc_pages_max)
         device_probe.alloc_pages_max = r->in.va_pages;
      unsigned index = device_probe.requests[DEV_ALLOC_EX]++;
      uint64_t in_flags = r->in.flags;
      int result;
      if (probe.live)
         result = host_ioctl(fd, request, arg);
      else {
         r->out.flags = in_flags;
         /* Distinct page-aligned cookies, as the kernel hands out. Sharing one
          * address across BOs makes every later alloc collide, and the driver
          * derives BO identity from the handle. The window is
          * [BASE_MEM_COOKIE_BASE, BASE_MEM_FIRST_FREE_ADDRESS), so the stride
          * has to stay small enough that a long init sequence still fits. */
         r->out.gpu_va = BASE_MEM_COOKIE_BASE + (uint64_t)index * 4096;
         result = 0;
      }

      return result;
   }
   if (request == KBASE_IOCTL_MEM_FREE) {
      const struct kbase_ioctl_mem_free *r = arg;
      if (!r->gpu_addr)
         return blocked();
      device_note(DEV_FREE);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      return result;
   }
   if (request == KBASE_IOCTL_MEM_SYNC) {
      const struct kbase_ioctl_mem_sync *r = arg;
      if (!r->handle || !r->size ||
          (r->type != BASE_SYNCSET_OP_MSYNC && r->type != BASE_SYNCSET_OP_CSYNC))
         return blocked();
      device_note(DEV_SYNC);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      if (!probe.live && !result && probe.submit_mode &&
          r->type == BASE_SYNCSET_OP_CSYNC && r->size >= 8 && r->user_addr) {
         /* There is no GPU here to advance the progress seqno, so
          * kbase_csf_wait would spin until its deadline and every kick would
          * surface as VK_ERROR_DEVICE_LOST -- the CPU run would never reach
          * the submit it is meant to exercise. A CSYNC invalidate is exactly
          * the point where the kernel would publish a completed job, so the
          * CPU harness reports completion there. This is a harness affordance
          * for the non-live path only and is never evidence about hardware. */
         uint64_t *cell = (uint64_t *)(uintptr_t)r->user_addr;
         if (*cell < UINT64_MAX)
            *cell = UINT64_MAX;
      }
      return result;
   }
   if (request == KBASE_IOCTL_MEM_ALIAS) {
      union kbase_ioctl_mem_alias *r = arg;
      if (!r->in.nents || r->in.nents > 4 || !r->in.stride || !r->in.aliasing_info)
         return blocked();
      device_note(DEV_ALIAS);
      uint64_t in_nents = r->in.nents, in_stride = r->in.stride;
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      if (!probe.live && !result) {
         /* kbase_kmod_alias_create rejects a zero cookie, a va_pages count
          * that does not match the total, and a missing NEED_MMAP flag, so the
          * CPU path has to report the same output shape the kernel does. */
         r->out.va_pages = r->in.stride * r->in.nents;
         r->out.flags = BASE_MEM_NEED_MMAP;
         /* The handle has to sit inside the guard's cookie window so the
          * alias view can be mapped, and must not collide with the BO cookies
          * handed out by MEM_ALLOC_EX. */
         r->out.gpu_va = BASE_MEM_COOKIE_BASE + 40 * 4096;
      }
      printf("DEVICE_ALIAS nents=%llu stride_pages=%llu ret=%d cookie=0x%llx\n",
             (unsigned long long)in_nents, (unsigned long long)in_stride, result,
             (unsigned long long)r->out.gpu_va);
      return result;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_REGISTER) {
      const struct kbase_ioctl_cs_queue_register *r = arg;
      if (!r->buffer_gpu_addr || r->buffer_size < 4096 || (r->buffer_size & (r->buffer_size - 1)))
         return blocked();
      device_note(DEV_QUEUE_REGISTER);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      if (device_probe.requests[DEV_QUEUE_REGISTER] <= 3)
         printf("DEVICE_QUEUE_REGISTER ring_va=0x%llx size=%u priority=%u ret=%d\n",
                (unsigned long long)r->buffer_gpu_addr, r->buffer_size, r->priority, result);
      return result;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18) {
      const union kbase_ioctl_cs_queue_group_create_1_18 *r = arg;
      if (r->in.priority > 3 || !r->in.cs_min)
         return blocked();
      device_note(DEV_GROUP_CREATE);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      return result;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_BIND) {
      union kbase_ioctl_cs_queue_bind *r = arg;
      if (!r->in.buffer_gpu_addr || r->in.csi_index)
         return blocked();
      device_note(DEV_QUEUE_BIND);
      uint64_t bind_ring = r->in.buffer_gpu_addr;
      uint8_t bind_group = r->in.group_handle;
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      if (!probe.live && !result) {
         /* The kernel hands back the CSF user-IO mmap handle, which the queue
          * then maps; echoing the ring VA would fall outside the guard's
          * cookie window and the mmap would be refused. */
         r->out.mmap_handle = BASEP_MEM_CSF_USER_IO_PAGES_HANDLE;
         device_probe.group_handles_valid++;
         printf("DEVICE_QUEUE_BIND ring_va=0x%llx group=%u map_handle=0x%llx ret=0\n",
                (unsigned long long)bind_ring, bind_group,
                (unsigned long long)r->out.mmap_handle);
         return result;
      }
      if (!result && r->out.mmap_handle) {
         device_probe.group_handles_valid++;
         printf("DEVICE_QUEUE_BIND ring_va=0x%llx group=%u map_handle=0x%llx ret=0\n",
                (unsigned long long)r->in.buffer_gpu_addr, r->in.group_handle,
                (unsigned long long)r->out.mmap_handle);
      }
      return result;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_KICK) {
      const struct kbase_ioctl_cs_queue_kick *r = arg;
      if (!r->buffer_gpu_addr)
         return blocked();
      device_note(DEV_QUEUE_KICK);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      printf("DEVICE_QUEUE_KICK ring_va=0x%llx index=%u ret=%d errno=%d\n",
             (unsigned long long)r->buffer_gpu_addr, device_probe.requests[DEV_QUEUE_KICK], result,
             result ? errno : 0);
      return result;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_TERMINATE) {
      const struct kbase_ioctl_cs_queue_terminate *r = arg;
      if (!r->buffer_gpu_addr)
         return blocked();
      device_note(DEV_QUEUE_TERM);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      return result;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE) {
      device_note(DEV_GROUP_TERM);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      return result;
   }
   if (request == KBASE_IOCTL_CS_TILER_HEAP_INIT) {
      union kbase_ioctl_cs_tiler_heap_init *r = arg;
      if (!r->in.chunk_size || !r->in.initial_chunks || !r->in.max_chunks ||
          r->in.initial_chunks > r->in.max_chunks || !r->in.target_in_flight)
         return blocked();
      device_note(DEV_HEAP_INIT);
      /* in and out share offset 0 in this union, so the input values have to
       * be captured before the CPU path fills the output fields. */
      uint32_t in_chunk = r->in.chunk_size, in_initial = r->in.initial_chunks,
               in_max = r->in.max_chunks;
      uint16_t in_target = r->in.target_in_flight;
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      if (!probe.live && !result) {
         /* kbase_kmod_heap_create rejects a zero context or first-chunk
          * address, so the CPU path must report page-aligned ones. The value
          * is never mapped, but it stays inside the cookie window so it cannot
          * be confused with a real handle. */
         unsigned index = device_probe.requests[DEV_HEAP_INIT];
         r->out.gpu_heap_va = BASE_MEM_COOKIE_BASE + 56 * 4096 + index * 4096;
         r->out.first_chunk_va = r->out.gpu_heap_va + 1024 * 1024;
      }
      if (!result)
         printf("DEVICE_HEAP_INIT chunk=%u initial=%u max=%u target=%u ret=0\n", in_chunk,
                in_initial, in_max, in_target);
      return result;
   }
   if (request == KBASE_IOCTL_CS_TILER_HEAP_TERM) {
      const struct kbase_ioctl_cs_tiler_heap_term *r = arg;
      if (!r->gpu_heap_va)
         return blocked();
      device_note(DEV_HEAP_TERM);
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      return result;
   }
   return blocked();
}

static void *
device_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
   const char *kind = NULL;
   if (address || !device_mmap_allowed(fd, size, prot, flags, offset, &kind)) {
      blocked();
      return MAP_FAILED;
   }
   void *ptr;
   /* The CPU device descriptor is a memfd sized for the alias fixtures, so
    * backing any of these mappings with it would fault on the driver's first
    * write. Anonymous memory stands in for the kernel's pages; the probe only
    * observes syscalls, never the contents. */
   if (probe.live)
      ptr = host_mmap(NULL, size, prot, flags, fd, offset);
   else
      ptr = host_mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (ptr != MAP_FAILED) {
      device_probe.maps++;
      device_probe.live_maps++;
      if (device_probe.map_ledger_count < DEVICE_MAP_LEDGER_MAX)
         device_probe.maps_ledger[device_probe.map_ledger_count++] =
            (struct device_map_entry){.address = ptr, .size = size, .live = true};
      else
         device_probe.map_ledger_overflow++;
   } else
      fprintf(stderr, "device mmap failed kind=%s size=%zu prot=%d offset=0x%llx errno=%d\n", kind,
              size, prot, (unsigned long long)offset, errno);
   return ptr;
}

static int
device_munmap(void *address, size_t size)
{
   /* Only mappings this device window created are accounted for; the process
    * also unmaps anonymous runtime memory, which must not be mistaken for a
    * leaked device mapping. */
   struct device_map_entry *entry = NULL;
   for (unsigned i = 0; i < device_probe.map_ledger_count; i++) {
      if (device_probe.maps_ledger[i].live && device_probe.maps_ledger[i].address == address) {
         entry = &device_probe.maps_ledger[i];
         break;
      }
   }
   if (entry && entry->size != size) {
      printf("DEVICE_UNMAP_SIZE_MISMATCH address=%p mapped=%zu unmapped=%zu\n", address,
             entry->size, size);
      device_probe.cleanup_uncertain = true;
   }
   int result = host_munmap(address, size);
   if (entry) {
      if (result) {
         /* A failed unmap leaves the device mapping live and the driver's own
          * cleanup-uncertain state unreachable from here (the ICD exports no
          * such symbol), so the ledger is the only place that can report it.
          * Counting it as released would hide exactly the cross-window leak. */
         printf("DEVICE_UNMAP_FAILED address=%p size=%zu errno=%d\n", address, size, errno);
         device_probe.cleanup_uncertain = true;
      } else {
         entry->live = false;
         device_probe.live_maps--;
         device_probe.unmaps++;
      }
   }
   return result;
}

static ssize_t
device_read(int fd, void *data, size_t size)
{
   if (!data || !size || size > 4096)
      return blocked();
   ssize_t got = host_read(fd, data, size);
   device_probe.reads++;
   return got;
}

static int
device_ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
             const sigset_t *mask)
{
   if (!fds || count != 1 || (fds[0].fd != probe.fd && !device_is_logical_fd(fds[0].fd)))
      return blocked();
   device_probe.polls++;
   return host_ppoll(fds, count, timeout, mask);
}

static void
device_print_ledger(void)
{
   printf("DEVICE_LEDGER");
   for (unsigned i = 0; i < DEV_REQUEST_COUNT; i++)
      printf(" %s=%u", device_request_names[i], device_probe.requests[i]);
   printf(" opens=%u maps=%u unmaps=%u reads=%u polls=%u closes=%u\n", probe.device_opens,
          device_probe.maps, device_probe.unmaps, device_probe.reads, device_probe.polls,
          probe.device_closes);
}

static bool
device_guard_self_test(void)
{
   bool ok = !probe.live;
   /* The guard still denies every open after the hand-off, so the single reopen
    * is served by the child's pre-opened descriptor rather than by a new open. */
   errno = 0;
   ok &= syscall(SYS_openat, AT_FDCWD, "/home/zl/github/mesa/VERSION", O_RDONLY, 0) == -1 &&
         errno == EPERM;
   errno = 0;
   ok &= syscall(SYS_openat, AT_FDCWD, "/proc/self/fd", O_RDONLY | O_CLOEXEC, 0) == -1 &&
         errno == EPERM;
   errno = 0;
   ok &= syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, 0) == -1 && errno == EPERM;
   const unsigned allowed[] = {
      KBASE_IOCTL_VERSION_CHECK_CSF, KBASE_IOCTL_SET_FLAGS, KBASE_IOCTL_GET_GPUPROPS,
      KBASE_IOCTL_CS_GET_GLB_IFACE, KBASE_IOCTL_MEM_JIT_INIT, KBASE_IOCTL_MEM_ALLOC_EX,
      KBASE_IOCTL_MEM_FREE, KBASE_IOCTL_MEM_SYNC, KBASE_IOCTL_MEM_ALIAS,
      KBASE_IOCTL_CS_QUEUE_REGISTER, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18,
      KBASE_IOCTL_CS_QUEUE_BIND, KBASE_IOCTL_CS_QUEUE_KICK, KBASE_IOCTL_CS_QUEUE_TERMINATE,
      KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, KBASE_IOCTL_CS_TILER_HEAP_INIT,
      KBASE_IOCTL_CS_TILER_HEAP_TERM, KBASE_IOCTL_MEM_IMPORT,
   };
   const unsigned denied[] = {
      KBASE_IOCTL_KCPU_QUEUE_CREATE, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, KBASE_IOCTL_KCPU_QUEUE_DELETE,
      KBASE_IOCTL_CS_EVENT_SIGNAL, KBASE_IOCTL_MEM_QUERY,
      KBASE_IOCTL_MEM_COMMIT, KBASE_IOCTL_MEM_EXEC_INIT, KBASE_IOCTL_MEM_ALLOC,
      KBASE_IOCTL_JOB_SUBMIT, KBASE_IOCTL_GET_CPU_GPU_TIMEINFO, KBASE_IOCTL_VERSION_CHECK_JM,
      KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6,
   };
   for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
      errno = 0;
      if (syscall(SYS_ioctl, probe.fd, allowed[i], 0) != -1 || errno != ENOTTY) {
         printf("DEVICE_GUARD_FAIL allowed fd=physical request=0x%x errno=%d\n", allowed[i], errno);
         ok = false;
      }
      errno = 0;
      if (syscall(SYS_ioctl, probe.reopen_fd, allowed[i], 0) != -1 || errno != ENOTTY) {
         printf("DEVICE_GUARD_FAIL allowed fd=logical request=0x%x errno=%d\n", allowed[i], errno);
         ok = false;
      }
   }
   for (unsigned i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
      errno = 0;
      if (syscall(SYS_ioctl, probe.fd, denied[i], 0) != -1 || errno != EPERM) {
         printf("DEVICE_GUARD_FAIL denied fd=physical request=0x%x errno=%d\n", denied[i], errno);
         ok = false;
      }
      errno = 0;
      if (syscall(SYS_ioctl, probe.reopen_fd, denied[i], 0) != -1 || errno != EPERM) {
         printf("DEVICE_GUARD_FAIL denied fd=logical request=0x%x errno=%d\n", denied[i], errno);
         ok = false;
      }
   }
   errno = 0;
   if (syscall(SYS_ioctl, -1, KBASE_IOCTL_MEM_ALLOC_EX, 0) != -1 || errno != EPERM) {
      printf("DEVICE_GUARD_FAIL fd=-1 errno=%d\n", errno);
      ok = false;
   }
   errno = 0;
   if (syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_MEM_ALLOC_EX ^ (1u << _IOC_SIZESHIFT), 0) != -1 ||
       errno != EPERM) {
      printf("DEVICE_GUARD_FAIL wrong-size errno=%d\n", errno);
      ok = false;
   }
   errno = 0;
   if (syscall(SYS_ioctl, device_probe.unused_fd, KBASE_IOCTL_MEM_ALLOC_EX, 0) != -1 ||
       errno != EPERM) {
      printf("DEVICE_GUARD_FAIL unrelated-fd errno=%d\n", errno);
      ok = false;
   }
   /* mmap envelope */
   const struct {
      size_t size;
      int prot;
      int flags;
      int fd;
      off_t offset;
      bool allow;
   } maps[] = {
      {4096, PROT_NONE, MAP_SHARED, probe.fd, BASE_MEM_MAP_TRACKING_HANDLE, true},
      {4096, PROT_NONE, MAP_SHARED, probe.reopen_fd, BASE_MEM_MAP_TRACKING_HANDLE, true},
      {4096, PROT_READ, MAP_SHARED, probe.reopen_fd, BASEP_MEM_CSF_USER_REG_PAGE_HANDLE, true},
      {12288, PROT_READ | PROT_WRITE, MAP_SHARED, probe.reopen_fd, BASEP_MEM_CSF_USER_IO_PAGES_HANDLE, true},
      {4096, PROT_READ | PROT_WRITE, MAP_SHARED, probe.reopen_fd, BASE_MEM_COOKIE_BASE, true},
      {1048576, PROT_READ, MAP_SHARED, probe.reopen_fd, BASE_MEM_COOKIE_BASE, true},
      {4096, PROT_NONE, MAP_SHARED, probe.reopen_fd, BASE_MEM_COOKIE_BASE, false},
      {4096, PROT_EXEC | PROT_READ, MAP_SHARED, probe.reopen_fd, BASE_MEM_COOKIE_BASE, false},
      {4096, PROT_READ, MAP_PRIVATE, probe.reopen_fd, BASE_MEM_COOKIE_BASE, false},
      {4096, PROT_READ, MAP_SHARED, probe.reopen_fd, BASE_MEM_FIRST_FREE_ADDRESS, false},
      {4096, PROT_READ, MAP_SHARED, probe.reopen_fd, BASE_MEM_COOKIE_BASE + 1, false},
      {DEVICE_COOKIE_MAX_SIZE + 4096, PROT_READ, MAP_SHARED, probe.reopen_fd, BASE_MEM_COOKIE_BASE, false},
      {4096, PROT_READ, MAP_SHARED, device_probe.unused_fd, BASE_MEM_COOKIE_BASE, false},
   };
   for (unsigned i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
      errno = 0;
      void *ptr = (void *)syscall(SYS_mmap, 0, maps[i].size, maps[i].prot, maps[i].flags, maps[i].fd,
                                  maps[i].offset);
      if (maps[i].allow) {
         if (ptr == MAP_FAILED) {
            printf("DEVICE_GUARD_FAIL mmap case=%u expected-allow size=%zu prot=%d offset=0x%llx "
                   "errno=%d\n", i, maps[i].size, maps[i].prot, (unsigned long long)maps[i].offset,
                   errno);
            ok = false;
         } else if (host_munmap(ptr, maps[i].size) != 0) {
            printf("DEVICE_GUARD_FAIL mmap case=%u munmap-of-allowed errno=%d\n", i, errno);
            ok = false;
         }
      } else if (ptr != MAP_FAILED || errno != EPERM) {
         printf("DEVICE_GUARD_FAIL mmap case=%u expected-deny size=%zu prot=%d flags=%d offset=0x%llx "
               "got=%s errno=%d\n", i, maps[i].size, maps[i].prot, maps[i].flags,
               (unsigned long long)maps[i].offset, ptr == MAP_FAILED ? "failed" : "mapped", errno);
         ok = false;
         if (ptr != MAP_FAILED)
            host_munmap(ptr, maps[i].size);
      }
   }
   printf("DEVICE_GUARD_CPU allowed=%u denied=%u envelope_checks=%u\n",
          (unsigned)(sizeof(allowed) / sizeof(allowed[0])),
          (unsigned)(sizeof(denied) / sizeof(denied[0])), (unsigned)(sizeof(maps) / sizeof(maps[0])));
   return ok;
}

static bool
device_sequence_self_test(void)
{
   CHECK(!probe.live);
   unsigned before = probe.blocked;
   /* Requests that belong to the logical device must be refused before it exists. */
   {
      int saved = probe.reopen_fd;
      probe.reopen_fd = -1;
      union kbase_ioctl_mem_alloc_ex alloc = {.in = {.va_pages = 1, .commit_pages = 1,
                                                     .flags = BASE_MEM_SAME_VA}};
      CHECK(device_ioctl(saved, KBASE_IOCTL_MEM_ALLOC_EX, &alloc) == -1 && errno == EPERM);
      probe.reopen_fd = saved;
   }
   /* Malformed shapes must be refused even on the logical fd. */
   {
      union kbase_ioctl_mem_alloc_ex alloc = {.in = {.va_pages = 2, .commit_pages = 1,
                                                     .flags = BASE_MEM_SAME_VA}};
      CHECK(device_ioctl(probe.reopen_fd, KBASE_IOCTL_MEM_ALLOC_EX, &alloc) == -1 && errno == EPERM);
      alloc = (union kbase_ioctl_mem_alloc_ex){.in = {.va_pages = 1, .commit_pages = 1,
                                                     .flags = BASE_MEM_PROTECTED | BASE_MEM_SAME_VA}};
      CHECK(device_ioctl(probe.reopen_fd, KBASE_IOCTL_MEM_ALLOC_EX, &alloc) == -1 && errno == EPERM);
      struct kbase_ioctl_cs_queue_kick kick = {0};
      CHECK(device_ioctl(probe.reopen_fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) == -1 && errno == EPERM);
      struct kbase_ioctl_cs_queue_register reg = {.buffer_gpu_addr = 0x1000, .buffer_size = 3000};
      CHECK(device_ioctl(probe.reopen_fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg) == -1 && errno == EPERM);
   }
   CHECK(probe.blocked == before + 5);
   probe.blocked = before;
   /* Folded rules are exercised: a canonical sequence must be accepted. */
   union kbase_ioctl_mem_alloc_ex alloc = {.in = {.va_pages = 1, .commit_pages = 1,
                                                  .flags = BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT}};
   CHECK(device_ioctl(probe.reopen_fd, KBASE_IOCTL_MEM_ALLOC_EX, &alloc) == 0);
   CHECK(device_probe.saw_csf_event && device_probe.saw_same_va && !device_probe.saw_gpu_ex);
   CHECK(device_probe.alloc_pages_max == 1);
   CHECK(device_probe.requests[DEV_ALLOC_EX] == 1);
   struct kbase_ioctl_cs_queue_kick kick = {.buffer_gpu_addr = 0x1000};
   CHECK(device_ioctl(probe.reopen_fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick) == 0);
   CHECK(device_probe.requests[DEV_QUEUE_KICK] == 1);
   printf("DEVICE_SEQUENCE_CPU sequence_rules=ok alloc=%u kick=%u\n",
          device_probe.requests[DEV_ALLOC_EX], device_probe.requests[DEV_QUEUE_KICK]);
   return true;
}

static bool
exercise_device(VkInstance instance, PFN_vkGetInstanceProcAddr gipa)
{
   CHECK(probe.device_mode && probe.active && !probe.closed);
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
   CHECK(gdpa && panvk_icd_symbol_in_image((void *)gipa, (void *)gdpa));
   PFN_vkEnumeratePhysicalDevices enumerate =
      (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
   CHECK(enumerate);
   uint32_t count = 0;
   CHECK(enumerate(instance, &count, NULL) == VK_SUCCESS && count == 1);
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   count = 1;
   CHECK(enumerate(instance, &count, &physical) == VK_SUCCESS && physical != VK_NULL_HANDLE);

   PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
   CHECK(create_device && panvk_icd_symbol_in_image((void *)gipa, (void *)create_device));
   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   VkDeviceCreateInfo create = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device = VK_NULL_HANDLE;
   VkResult result = create_device(physical, &create, NULL, &device);
   printf("DEVICE_CREATE result=%d handle=%s\n", result, device ? "non-null" : "null");
   if (result != VK_SUCCESS || !device)
      return false;
   device_probe.created = true;

   PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)gdpa(device, "vkGetDeviceQueue");
   PFN_vkDestroyDevice destroy_device = (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
   CHECK(get_queue && destroy_device &&
         panvk_icd_symbol_in_image((void *)gipa, (void *)get_queue) &&
         panvk_icd_symbol_in_image((void *)gipa, (void *)destroy_device));
   VkQueue queue = VK_NULL_HANDLE;
   get_queue(device, 0, 0, &queue);
   device_probe.queue_returned = queue != VK_NULL_HANDLE;
   printf("DEVICE_QUEUE handle=%s\n", queue ? "non-null" : "null");

   /* Submit mode only: build a PRIMARY command buffer with zero recorded
    * commands and submit it. EndCommandBuffer still emits a non-empty compute
    * stream (wait-slots + CS error save + cache flush), so this reaches
    * kbase_csf_submit and issues a real CS_QUEUE_KICK -- unlike the M5a path,
    * where the only kicks are the driver's internal init-stream submits. */
   if (probe.submit_mode && queue) {
      PFN_vkCreateCommandPool create_pool =
         (PFN_vkCreateCommandPool)gdpa(device, "vkCreateCommandPool");
      PFN_vkAllocateCommandBuffers alloc_buffers =
         (PFN_vkAllocateCommandBuffers)gdpa(device, "vkAllocateCommandBuffers");
      PFN_vkBeginCommandBuffer begin =
         (PFN_vkBeginCommandBuffer)gdpa(device, "vkBeginCommandBuffer");
      PFN_vkEndCommandBuffer end =
         (PFN_vkEndCommandBuffer)gdpa(device, "vkEndCommandBuffer");
      PFN_vkCreateSemaphore create_semaphore =
         (PFN_vkCreateSemaphore)gdpa(device, "vkCreateSemaphore");
      PFN_vkDestroySemaphore destroy_semaphore =
         (PFN_vkDestroySemaphore)gdpa(device, "vkDestroySemaphore");
      PFN_vkQueueSubmit submit_fn = (PFN_vkQueueSubmit)gdpa(device, "vkQueueSubmit");
      PFN_vkQueueWaitIdle wait_idle = (PFN_vkQueueWaitIdle)gdpa(device, "vkQueueWaitIdle");
      PFN_vkDestroyCommandPool destroy_pool =
         (PFN_vkDestroyCommandPool)gdpa(device, "vkDestroyCommandPool");
      CHECK(create_pool && alloc_buffers && begin && end && create_semaphore &&
            destroy_semaphore && submit_fn && wait_idle && destroy_pool);
      VkCommandPoolCreateInfo pool_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = 0,
      };
      VkCommandPool pool = VK_NULL_HANDLE;
      CHECK(create_pool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
      VkCommandBufferAllocateInfo cb_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      VkCommandBuffer cmd = VK_NULL_HANDLE;
      CHECK(alloc_buffers(device, &cb_info, &cmd) == VK_SUCCESS);
      VkCommandBufferBeginInfo begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
      };
      CHECK(begin(cmd, &begin_info) == VK_SUCCESS);
      CHECK(end(cmd) == VK_SUCCESS);
      VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphore semaphore = VK_NULL_HANDLE;
      CHECK(create_semaphore(device, &sem_info, NULL, &semaphore) == VK_SUCCESS);
      device_probe.submit_kicks_before = device_probe.requests[DEV_QUEUE_KICK];
      device_probe.submit_polls_before = device_probe.polls;
      device_probe.submit_reads_before = device_probe.reads;
      VkSubmitInfo submit_info = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
         .signalSemaphoreCount = 1,
         .pSignalSemaphores = &semaphore,
      };
      device_probe.submit_attempted = true;
      VkResult submit_result = submit_fn(queue, 1, &submit_info, VK_NULL_HANDLE);
      printf("DEVICE_SUBMIT result=%d kicks_before=%u\n", submit_result,
             device_probe.submit_kicks_before);
      device_probe.submit_accepted = submit_result == VK_SUCCESS;
      /* The semaphore signal is a userspace store on this backend (no ioctl),
       * so it is reported but carries no GPU evidence. */
      VkResult idle_result = wait_idle(queue);
      printf("DEVICE_SUBMIT_DRAIN result=%d\n", idle_result);
      device_probe.submit_drained = idle_result == VK_SUCCESS;
      destroy_semaphore(device, semaphore, NULL);
      /* The command buffer's CS pool owns a BO; leaving the pool alive would
       * show up as a leaked device mapping in the ledger. */
      destroy_pool(device, pool, NULL);
   }

   destroy_device(device, NULL);
   device_probe.destroyed = true;
   return true;
}

static bool
device_check_result(void)
{
   device_print_ledger();
   bool ok = device_probe.created && device_probe.queue_returned && device_probe.destroyed;
   ok &= device_probe.requests[DEV_QUEUE_REGISTER] >= 3;
   ok &= device_probe.requests[DEV_GROUP_CREATE] >= 3;
   ok &= device_probe.requests[DEV_QUEUE_BIND] >= 3;
   ok &= device_probe.requests[DEV_QUEUE_KICK] >= 3;
   ok &= device_probe.group_handles_valid >= 3;
   ok &= device_probe.requests[DEV_ALLOC_EX] >= 1;
   ok &= device_probe.requests[DEV_ALIAS] >= 1;
   ok &= device_probe.requests[DEV_SYNC] >= 1;
   ok &= device_probe.requests[DEV_JIT] >= 1;
   ok &= device_probe.requests[DEV_HEAP_INIT] >= 1;
   ok &= device_probe.requests[DEV_CSIF] >= 1;
   ok &= device_probe.saw_same_va;
   ok &= device_probe.maps >= 1 && !device_probe.cleanup_uncertain;
   ok &= device_probe.map_ledger_overflow == 0;
   ok &= device_probe.live_maps == 0 && device_probe.unmaps == device_probe.maps;
   ok &= probe.blocked == 0;
   if (probe.submit_mode) {
      /* The user submit must have added exactly one kick on top of the three
       * init-stream kicks, and kbase_csf_wait must have returned completion
       * (kbase_compute_execute maps a timeout or a lost queue to
       * VK_ERROR_DEVICE_LOST, so VK_SUCCESS means the seqno was reached). */
      ok &= device_probe.submit_attempted && device_probe.submit_accepted;
      ok &= device_probe.requests[DEV_QUEUE_KICK] == device_probe.submit_kicks_before + 1;
      ok &= device_probe.submit_drained;
   }
   printf("DEVICE_VERDICT accepted=%u created=%u queue=%u destroyed=%u kicks=%u registers=%u "
          "binds=%u alias=%u sync=%u jit=%u heaps=%u same_va=%u csf_event=%u gpu_ex=%u maps=%u "
          "unmaps=%u live_maps=%u ledger_overflow=%u reads=%u polls=%u blocked=%u "
          "cleanup_uncertain=%u gpu_completion_observed=%u submit=%u kick_delta=%d "
          "polls_delta=%d reads_delta=%d\n",
          ok, device_probe.created, device_probe.queue_returned, device_probe.destroyed,
          device_probe.requests[DEV_QUEUE_KICK], device_probe.requests[DEV_QUEUE_REGISTER],
          device_probe.requests[DEV_QUEUE_BIND], device_probe.requests[DEV_ALIAS],
          device_probe.requests[DEV_SYNC], device_probe.requests[DEV_JIT],
          device_probe.requests[DEV_HEAP_INIT], device_probe.saw_same_va, device_probe.saw_csf_event,
          device_probe.saw_gpu_ex, device_probe.maps, device_probe.unmaps, device_probe.live_maps,
          device_probe.map_ledger_overflow, device_probe.reads, device_probe.polls, probe.blocked,
          device_probe.cleanup_uncertain, device_probe.submit_drained,
          device_probe.submit_accepted,
          (int)device_probe.requests[DEV_QUEUE_KICK] - (int)device_probe.submit_kicks_before,
          (int)device_probe.polls - (int)device_probe.submit_polls_before,
          (int)device_probe.reads - (int)device_probe.submit_reads_before);
   return ok;
}
