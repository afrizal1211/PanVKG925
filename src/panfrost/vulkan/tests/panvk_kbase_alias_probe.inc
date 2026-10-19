#define ALIAS_BACKING_SIZE (512u * 1024u)
#define ALIAS_TOTAL_SIZE (2u * ALIAS_BACKING_SIZE)
#define ALIAS_BACKING_PAGES (ALIAS_BACKING_SIZE / 4096u)
#define ALIAS_BACKING_FLAGS (BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | \
                             BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | \
                             BASE_MEM_COHERENT_LOCAL | BASE_MEM_SAME_VA)
#define ALIAS_INPUT_FLAGS (BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR)

_Static_assert(ALIAS_BACKING_FLAGS == 0x280f, "alias backing flags");
_Static_assert(ALIAS_INPUT_FLAGS == 0xd, "alias input flags");
_Static_assert(sizeof(struct base_mem_aliasing_info) == 24, "alias entry ABI");
_Static_assert(sizeof(union kbase_ioctl_mem_alias) == 32, "alias ioctl ABI");
_Static_assert(offsetof(union kbase_ioctl_mem_alias, in.aliasing_info) == 24, "alias pointer ABI");
_Static_assert(KBASE_IOCTL_MEM_ALIAS == 0xc0208015u, "alias request ABI");

enum alias_operation {
   ALIAS_BACKING_ALLOC,
   ALIAS_BACKING_MAP,
   ALIAS_ALLOC,
   ALIAS_MAP,
   ALIAS_UNMAP,
   ALIAS_FREE,
   ALIAS_BACKING_UNMAP,
   ALIAS_BACKING_FREE,
   ALIAS_OPERATION_COUNT,
};

struct alias_resource {
   bool allocated;
   bool output_valid;
   uint64_t cookie;
   uint64_t flags;
   void *mapping;
};

static struct {
   bool active;
   bool cleanup_uncertain;
   unsigned guard_stage;
   struct alias_resource backing;
   struct alias_resource view;
   uint64_t output_pages;
   union kbase_ioctl_mem_alloc_ex backing_request;
   union kbase_ioctl_mem_alias request;
   struct base_mem_aliasing_info entries[2];
   unsigned calls[ALIAS_OPERATION_COUNT];
   int results[ALIAS_OPERATION_COUNT];
   int errors[ALIAS_OPERATION_COUNT];
   const char *reason;
   int cpu_fd;
   void *prepared_backing;
   void *prepared_view;
   unsigned prepared_released;
   unsigned delivered;
} alias_probe;

static bool install_guard(unsigned stage);

static bool
alias_single_window(uint64_t start, uint64_t size)
{
   return start && size && start <= UINTPTR_MAX && size - 1 <= UINTPTR_MAX - start &&
          (start >> 32) == ((start + size - 1) >> 32);
}

static void
alias_result(enum alias_operation operation, int result)
{
   alias_probe.results[operation] = result;
   alias_probe.errors[operation] = result ? errno : 0;
   printf("ALIAS_OPERATION index=%u ret=%d errno=%d calls=%u\n", operation, result,
          alias_probe.errors[operation], alias_probe.calls[operation]);
   if (result)
      errno = alias_probe.errors[operation];
}

static bool
alias_valid_flags(uint64_t flags, uint64_t required)
{
   uint64_t allowed = required | BASE_MEM_COHERENT_LOCAL | BASE_MEM_COHERENT_SYSTEM;
   return (flags & required) == required && !(flags & ~allowed);
}

static int
alias_ioctl(unsigned long request, void *arg)
{
   if (!probe.alias_mode || !alias_probe.active || probe.memory_mode || probe.csf_mode ||
       probe.props_calls != 2 || probe.maps != 1 || probe.unmaps || probe.closes)
      return blocked();
   if (request == KBASE_IOCTL_MEM_ALLOC_EX) {
      union kbase_ioctl_mem_alloc_ex *r = arg;
      const union kbase_ioctl_mem_alloc_ex expected = {
         .in = {.va_pages = ALIAS_BACKING_PAGES, .commit_pages = ALIAS_BACKING_PAGES,
                .flags = ALIAS_BACKING_FLAGS},
      };
      if (r != &alias_probe.backing_request || alias_probe.calls[ALIAS_BACKING_ALLOC] ||
          memcmp(r, &expected, sizeof(expected)))
         return blocked();
      alias_probe.calls[ALIAS_BACKING_ALLOC]++;
      int result;
      if (probe.live)
         result = host_ioctl(probe.fd, request, r);
      else if (cpu_case("backing-allocation-fail")) {
         errno = ENOMEM;
         result = -1;
      } else {
         r->out.flags = ALIAS_BACKING_FLAGS;
         r->out.gpu_va = cpu_case("backing-invalid-cookie") ? 0x1003 : BASE_MEM_COOKIE_BASE;
         if (cpu_case("backing-cached"))
            r->out.flags |= BASE_MEM_CACHED_CPU;
         result = 0;
      }
      alias_result(ALIAS_BACKING_ALLOC, result);
      if (!result) {
         alias_probe.backing.allocated = true;
         alias_probe.backing.cookie = r->out.gpu_va;
         alias_probe.backing.flags = r->out.flags;
      }
      printf("ALIAS_BACKING_ALLOC request=0x%lx bytes=%u pages=%u commit=%u flags_in=0x%llx flags_out=0x%llx cookie=0x%llx output_authoritative=%u\n",
             request, ALIAS_BACKING_SIZE, ALIAS_BACKING_PAGES, ALIAS_BACKING_PAGES,
             (unsigned long long)ALIAS_BACKING_FLAGS, (unsigned long long)r->out.flags,
             (unsigned long long)r->out.gpu_va, !result);
      return result;
   }
   if (request == KBASE_IOCTL_MEM_ALIAS) {
      union kbase_ioctl_mem_alias *r = arg;
      const union kbase_ioctl_mem_alias expected = {
         .in = {.flags = ALIAS_INPUT_FLAGS, .stride = ALIAS_BACKING_PAGES, .nents = 2,
                .aliasing_info = (uintptr_t)alias_probe.entries},
      };
      const struct base_mem_aliasing_info entry = {
         .handle = (uintptr_t)alias_probe.backing.mapping, .offset = 0, .length = ALIAS_BACKING_PAGES,
      };
      if (r != &alias_probe.request || !alias_probe.backing.mapping ||
          !alias_probe.backing.output_valid || alias_probe.guard_stage != 1 ||
          alias_probe.calls[ALIAS_ALLOC] || memcmp(r, &expected, sizeof(expected)) ||
          memcmp(&alias_probe.entries[0], &entry, sizeof(entry)) ||
          memcmp(&alias_probe.entries[1], &entry, sizeof(entry)))
         return blocked();
      alias_probe.calls[ALIAS_ALLOC]++;
      printf("ALIAS_INPUT request=0x%lx flags=0x%llx stride_pages=%u nents=2 entry_bytes=24 handle0=0x%llx handle1=0x%llx offset_pages=0 length_pages=%u\n",
             request, (unsigned long long)ALIAS_INPUT_FLAGS, ALIAS_BACKING_PAGES,
             (unsigned long long)entry.handle, (unsigned long long)entry.handle, ALIAS_BACKING_PAGES);
      int result;
      if (probe.live)
         result = host_ioctl(probe.fd, request, r);
      else if (cpu_case("alias-ioctl-fail")) {
         errno = ENOMEM;
         result = -1;
      } else {
         *r = (union kbase_ioctl_mem_alias){
            .out = {.flags = ALIAS_INPUT_FLAGS | BASE_MEM_NEED_MMAP,
                    .gpu_va = cpu_case("alias-invalid-cookie") ? 0x1003 :
                              cpu_case("different-cookie") ? BASE_MEM_COOKIE_BASE + 8192 : BASE_MEM_COOKIE_BASE,
                    .va_pages = cpu_case("alias-invalid-pages") ? 128 : 256},
         };
         if (cpu_case("alias-cached"))
            r->out.flags |= BASE_MEM_CACHED_CPU;
         if (cpu_case("alias-missing-need-mmap"))
            r->out.flags &= ~BASE_MEM_NEED_MMAP;
         if (cpu_case("alias-same-va-only"))
            r->out.flags = (r->out.flags & ~BASE_MEM_NEED_MMAP) | BASE_MEM_SAME_VA;
         result = 0;
      }
      alias_result(ALIAS_ALLOC, result);
      if (!result) {
         alias_probe.view.allocated = true;
         alias_probe.view.cookie = r->out.gpu_va;
         alias_probe.view.flags = r->out.flags;
         alias_probe.output_pages = r->out.va_pages;
      }
      printf("ALIAS_OUTPUT ret=%d flags=0x%llx cookie=0x%llx va_pages=%llu output_authoritative=%u\n",
             result, (unsigned long long)r->out.flags, (unsigned long long)r->out.gpu_va,
             (unsigned long long)r->out.va_pages, !result);
      return result;
   }
   if (request == KBASE_IOCTL_MEM_FREE) {
      struct kbase_ioctl_mem_free *r = arg;
      struct alias_resource *resource;
      enum alias_operation operation, map_operation;
      if (r->gpu_addr == alias_probe.view.cookie && alias_probe.view.allocated) {
         resource = &alias_probe.view;
         operation = ALIAS_FREE;
         map_operation = ALIAS_MAP;
      } else if (r->gpu_addr == alias_probe.backing.cookie && alias_probe.backing.allocated &&
                 !alias_probe.view.allocated && !alias_probe.view.mapping) {
         resource = &alias_probe.backing;
         operation = ALIAS_BACKING_FREE;
         map_operation = ALIAS_BACKING_MAP;
      } else
         return blocked();
      if (!resource->output_valid || !valid_memory_cookie(r->gpu_addr) || resource->mapping ||
          alias_probe.calls[operation] || alias_probe.calls[map_operation] != 1 ||
          alias_probe.results[map_operation] != -1)
         return blocked();
      alias_probe.calls[operation]++;
      int result;
      if (probe.live)
         result = host_ioctl(probe.fd, request, r);
      else if ((operation == ALIAS_FREE && cpu_case("alias-free-fail")) ||
               (operation == ALIAS_BACKING_FREE && cpu_case("backing-free-fail"))) {
         errno = EIO;
         result = -1;
      } else
         result = 0;
      alias_result(operation, result);
      if (!result)
         resource->allocated = false;
      else
         alias_probe.cleanup_uncertain = true;
      printf("ALIAS_FREE_COOKIE kind=%s cookie=0x%llx ret=%d never_mapped=true\n",
             operation == ALIAS_FREE ? "alias" : "backing", (unsigned long long)r->gpu_addr, result);
      return result;
   }
   return blocked();
}

static void *
alias_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
   struct alias_resource *resource;
   enum alias_operation operation;
   if (size == ALIAS_BACKING_SIZE) {
      resource = &alias_probe.backing;
      operation = ALIAS_BACKING_MAP;
   } else if (size == ALIAS_TOTAL_SIZE) {
      resource = &alias_probe.view;
      operation = ALIAS_MAP;
   } else {
      blocked();
      return MAP_FAILED;
   }
   unsigned required_stage = operation == ALIAS_MAP ? 2 : 1;
   int required_prot = operation == ALIAS_MAP ? PROT_READ : PROT_READ | PROT_WRITE;
   if (!alias_probe.active || !resource->allocated || !resource->output_valid ||
       resource->mapping || alias_probe.calls[operation] || alias_probe.guard_stage != required_stage ||
       fd != probe.fd || probe.closed || address || size != (operation == ALIAS_MAP ? ALIAS_TOTAL_SIZE : ALIAS_BACKING_SIZE) ||
       prot != required_prot || flags != MAP_SHARED || offset != (off_t)resource->cookie) {
      blocked();
      return MAP_FAILED;
   }
   alias_probe.calls[operation]++;
   void *ptr;
   if (probe.live)
      ptr = host_mmap(NULL, size, prot, flags, fd, offset);
   else if ((operation == ALIAS_BACKING_MAP && (cpu_case("backing-mapping-fail") || cpu_case("backing-free-fail"))) ||
            (operation == ALIAS_MAP && (cpu_case("alias-mapping-fail") || cpu_case("alias-free-fail")))) {
      errno = ENOMEM;
      ptr = MAP_FAILED;
   } else {
      ptr = operation == ALIAS_MAP ? alias_probe.prepared_view : alias_probe.prepared_backing;
      CHECK(ptr && ptr != MAP_FAILED);
      alias_probe.delivered++;
   }
   alias_result(operation, ptr == MAP_FAILED ? -1 : 0);
   if (ptr != MAP_FAILED)
      resource->mapping = ptr;
   printf("ALIAS_MAPPING kind=%s bytes=%zu prot=%d cookie=0x%llx cpu_gpu_va=0x%llx mapped=%u errno=%d\n",
          operation == ALIAS_MAP ? "alias" : "backing", size, prot,
          (unsigned long long)resource->cookie, (unsigned long long)(uintptr_t)ptr,
          ptr != MAP_FAILED, ptr == MAP_FAILED ? errno : 0);
   return ptr;
}

static int
alias_munmap(void *address, size_t size)
{
   bool view = address == alias_probe.view.mapping;
   struct alias_resource *resource = view ? &alias_probe.view : &alias_probe.backing;
   enum alias_operation operation = view ? ALIAS_UNMAP : ALIAS_BACKING_UNMAP;
   if (!alias_probe.active || probe.closed || !address || address != resource->mapping ||
       size != (view ? ALIAS_TOTAL_SIZE : ALIAS_BACKING_SIZE) || alias_probe.calls[operation] ||
       (!view && (alias_probe.view.allocated || alias_probe.view.mapping)))
      return blocked();
   alias_probe.calls[operation]++;
   int result;
   if (!probe.live && ((view && cpu_case("alias-unmap-fail")) || (!view && cpu_case("backing-unmap-fail")))) {
      errno = EIO;
      result = -1;
   } else
      result = host_munmap(address, size);
   alias_result(operation, result);
   if (!result) {
      resource->mapping = NULL;
      resource->allocated = false;
      if (!probe.live) {
         if (view)
            alias_probe.prepared_view = NULL;
         else
            alias_probe.prepared_backing = NULL;
         alias_probe.prepared_released++;
      }
   } else
      alias_probe.cleanup_uncertain = true;
   printf("ALIAS_UNMAP kind=%s ret=%d no_mem_free_after_mapped_same_va=true\n", view ? "alias" : "backing", result);
   return result;
}

static void
alias_prepare_cpu(void)
{
   alias_probe.cpu_fd = memfd_create("panvk-alias-backing", MFD_CLOEXEC);
   CHECK(alias_probe.cpu_fd >= 0 && ftruncate(alias_probe.cpu_fd, ALIAS_BACKING_SIZE + 4096) == 0);
   alias_probe.prepared_backing = host_mmap(NULL, ALIAS_BACKING_SIZE, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, alias_probe.cpu_fd, 0);
   CHECK(alias_probe.prepared_backing != MAP_FAILED);
   void *view = host_mmap(NULL, ALIAS_TOTAL_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   CHECK(view != MAP_FAILED);
   CHECK(host_mmap(view, ALIAS_BACKING_SIZE, PROT_READ, MAP_FIXED | MAP_SHARED,
                   alias_probe.cpu_fd, 0) == view);
   void *second = (uint8_t *)view + ALIAS_BACKING_SIZE;
   CHECK(host_mmap(second, ALIAS_BACKING_SIZE, PROT_READ, MAP_FIXED | MAP_SHARED,
                   alias_probe.cpu_fd, cpu_case("view-mismatch") ? 4096 : 0) == second);
   alias_probe.prepared_view = view;
   printf("ALIAS_CPU_PREPARED source_views=1 repeated_views=2 prepared_owners=2 delivered=0 shared_memfd=true\n");
}

static void
alias_cleanup_cpu(void)
{
   if (alias_probe.prepared_view && alias_probe.prepared_view != alias_probe.view.mapping) {
      CHECK(host_munmap(alias_probe.prepared_view, ALIAS_TOTAL_SIZE) == 0);
      alias_probe.prepared_view = NULL;
      alias_probe.prepared_released++;
   }
   if (alias_probe.prepared_backing && alias_probe.prepared_backing != alias_probe.backing.mapping) {
      CHECK(host_munmap(alias_probe.prepared_backing, ALIAS_BACKING_SIZE) == 0);
      alias_probe.prepared_backing = NULL;
      alias_probe.prepared_released++;
   }
   CHECK(host_close(alias_probe.cpu_fd) == 0);
   unsigned retained = !!alias_probe.prepared_backing + !!alias_probe.prepared_view;
   CHECK(retained + alias_probe.prepared_released == 2);
   printf("ALIAS_CPU_PREPARED_CLEANUP delivered=%u released_owners=%u retained_owners=%u child_exit_isolation=true\n",
          alias_probe.delivered, alias_probe.prepared_released, retained);
}

static uint8_t
alias_pattern(unsigned index, unsigned generation)
{
   return (uint8_t)((index * 37u) ^ (index >> 7) ^ (index >> 15) ^ (generation * 0x5bu) ^ 0xa5u);
}

static void
alias_write_pattern(unsigned generation)
{
   volatile uint8_t *source = alias_probe.backing.mapping;
   for (unsigned i = 0; i < ALIAS_BACKING_SIZE; i++)
      source[i] = alias_pattern(i, generation);
   __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static sigjmp_buf alias_fault_jmp;
static volatile uint64_t alias_fault_address;
static struct sigaction alias_old_bus, alias_old_segv;

static void
alias_fault_handler(int signal_number, siginfo_t *info, void *context)
{
   (void)signal_number;
   (void)context;
   alias_fault_address = (uint64_t)(uintptr_t)info->si_addr;
   siglongjmp(alias_fault_jmp, 1);
}

static bool
alias_try_read(const volatile uint8_t *address, uint8_t *value, uint64_t *fault_address)
{
   alias_fault_address = 0;
   if (sigsetjmp(alias_fault_jmp, 1)) {
      *fault_address = alias_fault_address;
      return false;
   }
   *value = *address;
   return true;
}

static bool
alias_read_pattern(unsigned generation)
{
   const volatile uint8_t *view = alias_probe.view.mapping;
   const unsigned source_pages = ALIAS_BACKING_SIZE / 4096u;
   const unsigned total_pages = ALIAS_TOTAL_SIZE / 4096u;
   printf("ALIAS_READ_BEGIN generation=%u bytes=%u first_entry_bytes=%u\n",
          generation, ALIAS_TOTAL_SIZE, ALIAS_BACKING_SIZE);
   unsigned readable_pages = 0, mismatched = 0, first_fault_page = total_pages;
   uint64_t first_fault_address = 0;
   for (unsigned page = 0; page < total_pages; page++) {
      const volatile uint8_t *base = view + (size_t)page * 4096u;
      bool faulted = false;
      unsigned step = page < source_pages ? 1u : 512u;
      for (unsigned offset = 0; offset < 4096u; offset += step) {
         uint8_t value = 0;
         uint64_t fault = 0;
         if (!alias_try_read(base + offset, &value, &fault)) {
            faulted = true;
            if (first_fault_page == total_pages) {
               first_fault_page = page;
               first_fault_address = fault;
            }
            break;
         }
         if (value != alias_pattern((page % source_pages) * 4096u + offset, generation))
            mismatched++;
      }
      if (!faulted)
         readable_pages++;
   }
   blake3_hash hash;
   char hex[BLAKE3_HEX_LEN];
   _mesa_blake3_compute(alias_probe.backing.mapping, ALIAS_BACKING_SIZE, hash);
   _mesa_blake3_format(hex, hash);
   unsigned mapped_cpu_pages = first_fault_page < total_pages ? first_fault_page : total_pages;
   bool first_entry_readable = readable_pages >= source_pages && first_fault_page >= source_pages;
   printf("ALIAS_CPU_EXTENT generation=%u readable_pages=%u total_pages=%u cpu_mapped_pages=%u first_fault_page=%u first_fault_addr=0x%llx mismatched_samples=%u first_entry_fully_readable=%u source_blake3=%s cpu_full_view_supported=%u gpu_visibility_unproven=true\n",
          generation, readable_pages, total_pages, mapped_cpu_pages,
          first_fault_page, (unsigned long long)first_fault_address, mismatched,
          first_entry_readable, hex, mapped_cpu_pages == total_pages);
   if (mismatched || !first_entry_readable)
      return false;
   return true;
}

static void
alias_guard_self_test(unsigned stage)
{
   CHECK(!probe.live);
   const unsigned forbidden[] = {KBASE_IOCTL_MEM_SYNC, KBASE_IOCTL_CS_GET_GLB_IFACE,
                                 KBASE_IOCTL_KCPU_QUEUE_ENQUEUE};
   for (unsigned i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
      errno = 0;
      CHECK(syscall(SYS_ioctl, probe.fd, forbidden[i], 0) == -1 && errno == EPERM);
   }
   errno = 0;
   CHECK(syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_MEM_ALIAS, 0) == -1 && errno == ENOTTY);
   errno = 0;
   CHECK(syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_MEM_ALIAS ^ (1u << _IOC_SIZESHIFT), 0) == -1 && errno == EPERM);
   errno = 0;
   CHECK(syscall(SYS_ioctl, probe.fd, (uint64_t)KBASE_IOCTL_MEM_ALIAS | (1ull << 32), 0) == -1 && errno == EPERM);
   uint64_t cookies[] = {BASE_MEM_COOKIE_BASE, BASE_MEM_COOKIE_BASE + 4096, BASE_MEM_COOKIE_BASE + 8192};
   for (unsigned kind = 0; kind < 2; kind++) {
      size_t size = kind ? ALIAS_TOTAL_SIZE : ALIAS_BACKING_SIZE;
      int prot = kind ? PROT_READ : PROT_READ | PROT_WRITE;
      for (unsigned i = 0; i < 3; i++) {
         bool allowed = kind ? stage < 2 || cookies[i] == alias_probe.view.cookie :
                               !stage || cookies[i] == alias_probe.backing.cookie;
         errno = 0;
         void *ptr = (void *)syscall(SYS_mmap, 0, size, prot, MAP_SHARED, probe.fd, cookies[i]);
         if (allowed) {
            CHECK(ptr != MAP_FAILED);
            CHECK(((const volatile uint8_t *)ptr)[size - 1] == 0);
            CHECK(host_munmap(ptr, size) == 0);
         } else
            CHECK(ptr == MAP_FAILED && errno == EPERM);
      }
      uint64_t cookie = kind && stage == 2 ? alias_probe.view.cookie :
                        !kind && stage ? alias_probe.backing.cookie : BASE_MEM_COOKIE_BASE;
      const uint64_t wrong[][6] = {
         {0, size - 4096, prot, MAP_SHARED, probe.fd, cookie},
         {0, size, PROT_EXEC | prot, MAP_SHARED, probe.fd, cookie},
         {0, size, prot, MAP_PRIVATE, probe.fd, cookie},
         {0, size, prot, MAP_SHARED | MAP_FIXED, probe.fd, cookie},
         {4096, size, prot, MAP_SHARED, probe.fd, cookie},
         {0, size, prot, MAP_SHARED, (uint64_t)(uint32_t)probe.fd | (1ull << 32), cookie},
         {0, size, prot, MAP_SHARED, probe.fd, BASE_MEM_FIRST_FREE_ADDRESS},
         {0, size, prot, MAP_SHARED, probe.fd, cookie + 1},
         {0, size, prot, MAP_SHARED, probe.fd, cookie | (1ull << 32)},
         {0, size, kind ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, probe.fd, cookie},
         {0, (uint64_t)size | (1ull << 32), prot, MAP_SHARED, probe.fd, cookie},
      };
      for (unsigned i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
         errno = 0;
         CHECK(syscall(SYS_mmap, wrong[i][0], wrong[i][1], wrong[i][2], wrong[i][3], wrong[i][4], wrong[i][5]) == -1 && errno == EPERM);
      }
   }
   printf("ALIAS_GUARD_CPU stage=%u exact_backing=%u exact_alias=%u raw_checks_on_owned_memfd_only=true\n", stage, stage >= 1, stage == 2);
}

static void
alias_wrapper_self_test(void)
{
   CHECK(!probe.live && alias_probe.active);
   unsigned denied = probe.blocked;
   union kbase_ioctl_mem_alloc_ex saved = alias_probe.backing_request;
   alias_probe.backing_request.in.commit_pages--;
   CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_ALLOC_EX, &alias_probe.backing_request) == -1 && errno == EPERM);
   alias_probe.backing_request = saved;
   CHECK(!alias_probe.calls[ALIAS_BACKING_ALLOC]);
   union kbase_ioctl_mem_alias wrong = {0};
   CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_ALIAS, &wrong) == -1 && errno == EPERM);
   CHECK(!alias_probe.calls[ALIAS_ALLOC]);
   CHECK(probe.blocked == denied + 2);
   probe.blocked = denied;
   CHECK(alias_single_window(0x100000000ull, ALIAS_TOTAL_SIZE));
   CHECK(alias_single_window(0x1fff00000ull, ALIAS_TOTAL_SIZE));
   CHECK(!alias_single_window(0x1fff00001ull, ALIAS_TOTAL_SIZE));
   CHECK(!alias_single_window(0, ALIAS_TOTAL_SIZE));
   CHECK(!alias_single_window(UINT64_MAX - 4095, ALIAS_TOTAL_SIZE));
}

static bool
exercise_alias(void)
{
   CHECK(probe.alias_mode && !probe.csf_mode && !probe.memory_mode && probe.handed_off && !probe.closed);
   sigset_t mask;
   sigfillset(&mask);
   CHECK(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);
   alias_probe.active = true;
   alias_probe.reason = "backing-allocation-failed";
   alias_probe.backing_request = (union kbase_ioctl_mem_alloc_ex){
      .in = {.va_pages = ALIAS_BACKING_PAGES, .commit_pages = ALIAS_BACKING_PAGES, .flags = ALIAS_BACKING_FLAGS},
   };
   bool accepted = false;
   if (!probe.live)
      alias_wrapper_self_test();
   if (ioctl(probe.fd, KBASE_IOCTL_MEM_ALLOC_EX, &alias_probe.backing_request))
      goto out;
   alias_probe.reason = "backing-invalid-output";
   if (!valid_memory_cookie(alias_probe.backing.cookie) ||
       !alias_valid_flags(alias_probe.backing.flags, ALIAS_BACKING_FLAGS & ~BASE_MEM_COHERENT_LOCAL))
      goto out;
   alias_probe.backing.output_valid = true;
   alias_probe.reason = "backing-guard-failed";
   if (cpu_case("backing-guard-fail") || !install_guard(1))
      goto out;
   alias_probe.guard_stage = 1;
   if (!probe.live)
      alias_guard_self_test(1);
   alias_probe.reason = "backing-mapping-failed";
   if (mmap(NULL, ALIAS_BACKING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, probe.fd,
            alias_probe.backing.cookie) == MAP_FAILED)
      goto out;
   alias_write_pattern(0);
   printf("ALIAS_SOURCE_WRITTEN generation=0 bytes=%u source_va=0x%llx\n", ALIAS_BACKING_SIZE,
          (unsigned long long)(uintptr_t)alias_probe.backing.mapping);
   for (unsigned i = 0; i < 2; i++)
      alias_probe.entries[i] = (struct base_mem_aliasing_info){
         .handle = (uintptr_t)alias_probe.backing.mapping, .offset = 0, .length = ALIAS_BACKING_PAGES,
      };
   alias_probe.request = (union kbase_ioctl_mem_alias){
      .in = {.flags = ALIAS_INPUT_FLAGS, .stride = ALIAS_BACKING_PAGES, .nents = 2,
             .aliasing_info = (uintptr_t)alias_probe.entries},
   };
   if (!probe.live) {
      unsigned denied = probe.blocked;
      alias_probe.entries[1].handle = alias_probe.backing.cookie;
      CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_ALIAS, &alias_probe.request) == -1 && errno == EPERM);
      alias_probe.entries[1].handle = (uintptr_t)alias_probe.backing.mapping;
      alias_probe.entries[1].length *= 4096;
      CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_ALIAS, &alias_probe.request) == -1 && errno == EPERM);
      alias_probe.entries[1].length = ALIAS_BACKING_PAGES;
      CHECK(probe.blocked == denied + 2 && !alias_probe.calls[ALIAS_ALLOC]);
      probe.blocked = denied;
   }
   alias_probe.reason = "alias-ioctl-failed";
   if (ioctl(probe.fd, KBASE_IOCTL_MEM_ALIAS, &alias_probe.request))
      goto out;
   alias_probe.reason = "alias-invalid-output";
   if (!valid_memory_cookie(alias_probe.view.cookie) ||
       alias_probe.output_pages != ALIAS_TOTAL_SIZE / 4096 ||
       !alias_valid_flags(alias_probe.view.flags, ALIAS_INPUT_FLAGS | BASE_MEM_NEED_MMAP))
      goto out;
   alias_probe.view.output_valid = true;
   alias_probe.reason = "alias-guard-failed";
   if (cpu_case("alias-guard-fail") || !install_guard(2))
      goto out;
   alias_probe.guard_stage = 2;
   if (!probe.live)
      alias_guard_self_test(2);
   alias_probe.reason = "alias-mapping-failed";
   if (mmap(NULL, ALIAS_TOTAL_SIZE, PROT_READ, MAP_SHARED, probe.fd,
            alias_probe.view.cookie) == MAP_FAILED)
      goto out;
   alias_probe.reason = "single-window-policy";
   uint64_t observed = (uintptr_t)alias_probe.view.mapping;
   uint64_t checked = cpu_case("cross-window") ? 0x1fffff000ull : observed;
   printf("ALIAS_ADDRESS observed_start=0x%llx observed_end=0x%llx single_4g_window=%u policy_checked_start=0x%llx cpu_boundary_fixture=%u\n",
          (unsigned long long)observed, (unsigned long long)(observed + ALIAS_TOTAL_SIZE - 1),
          alias_single_window(observed, ALIAS_TOTAL_SIZE), (unsigned long long)checked, cpu_case("cross-window"));
   if (!alias_single_window(checked, ALIAS_TOTAL_SIZE))
      goto out;
   alias_probe.reason = "view-mismatch";
   struct sigaction action = {.sa_sigaction = alias_fault_handler, .sa_flags = SA_SIGINFO};
   CHECK(sigemptyset(&action.sa_mask) == 0);
   CHECK(sigaction(SIGBUS, &action, &alias_old_bus) == 0);
   CHECK(sigaction(SIGSEGV, &action, &alias_old_segv) == 0);
   sigset_t alias_fault_mask;
   CHECK(sigemptyset(&alias_fault_mask) == 0);
   CHECK(sigaddset(&alias_fault_mask, SIGBUS) == 0);
   CHECK(sigaddset(&alias_fault_mask, SIGSEGV) == 0);
   CHECK(sigprocmask(SIG_UNBLOCK, &alias_fault_mask, NULL) == 0);
   bool pattern_ok = alias_read_pattern(0);
   if (pattern_ok) {
      alias_write_pattern(1);
      pattern_ok = alias_read_pattern(1);
   }
   CHECK(sigprocmask(SIG_BLOCK, &alias_fault_mask, NULL) == 0);
   CHECK(sigaction(SIGBUS, &alias_old_bus, NULL) == 0);
   CHECK(sigaction(SIGSEGV, &alias_old_segv, NULL) == 0);
   if (!pattern_ok)
      goto out;
   accepted = true;
   alias_probe.reason = "accepted";
 out:
   if (alias_probe.view.mapping) {
      if (munmap(alias_probe.view.mapping, ALIAS_TOTAL_SIZE)) {
         alias_probe.reason = "alias-unmap-failed";
         accepted = false;
      }
   } else if (alias_probe.view.allocated && alias_probe.view.output_valid &&
              alias_probe.calls[ALIAS_MAP] == 1 && alias_probe.results[ALIAS_MAP] == -1) {
      struct kbase_ioctl_mem_free r = {.gpu_addr = alias_probe.view.cookie};
      if (ioctl(probe.fd, KBASE_IOCTL_MEM_FREE, &r))
         alias_probe.reason = "alias-unmapped-free-failed";
   }
   if (alias_probe.view.allocated || alias_probe.view.mapping) {
      alias_probe.cleanup_uncertain = true;
   } else if (alias_probe.backing.mapping) {
      if (munmap(alias_probe.backing.mapping, ALIAS_BACKING_SIZE)) {
         alias_probe.reason = "backing-unmap-failed";
         accepted = false;
      }
   } else if (alias_probe.backing.allocated && alias_probe.backing.output_valid &&
              alias_probe.calls[ALIAS_BACKING_MAP] == 1 && alias_probe.results[ALIAS_BACKING_MAP] == -1) {
      struct kbase_ioctl_mem_free r = {.gpu_addr = alias_probe.backing.cookie};
      if (ioctl(probe.fd, KBASE_IOCTL_MEM_FREE, &r))
         alias_probe.reason = "backing-unmapped-free-failed";
   }
   alias_probe.cleanup_uncertain |= alias_probe.backing.allocated || alias_probe.view.allocated;
   alias_probe.active = false;
   printf("ALIAS_VERDICT accepted=%u reason=%s guard_stage=%u cleanup_uncertain=%u backing_allocated=%u alias_allocated=%u backing_mapped=%u alias_mapped=%u gpu_execution_unproven=true\n",
          accepted, alias_probe.reason, alias_probe.guard_stage, alias_probe.cleanup_uncertain,
          alias_probe.backing.allocated, alias_probe.view.allocated,
          !!alias_probe.backing.mapping, !!alias_probe.view.mapping);
   return accepted;
}

static const struct {
   const char *name;
   const char *reason;
   unsigned calls[ALIAS_OPERATION_COUNT];
   unsigned guard_stage;
   bool uncertain;
   bool backing_allocated;
   bool view_allocated;
   bool backing_mapped;
   bool view_mapped;
} alias_fixtures[] = {
   {"positive", "accepted", {1, 1, 1, 1, 1, 0, 1, 0}, 2},
   {"different-cookie", "accepted", {1, 1, 1, 1, 1, 0, 1, 0}, 2},
   {"backing-allocation-fail", "backing-allocation-failed", {1}, 0},
   {"backing-mapping-fail", "backing-mapping-failed", {1, 1, 0, 0, 0, 0, 0, 1}, 1},
   {"backing-invalid-cookie", "backing-invalid-output", {1}, 0, true, true},
   {"backing-cached", "backing-invalid-output", {1}, 0, true, true},
   {"alias-ioctl-fail", "alias-ioctl-failed", {1, 1, 1, 0, 0, 0, 1, 0}, 1},
   {"alias-mapping-fail", "alias-mapping-failed", {1, 1, 1, 1, 0, 1, 1, 0}, 2},
   {"alias-invalid-cookie", "alias-invalid-output", {1, 1, 1}, 1, true, true, true, true},
   {"alias-invalid-pages", "alias-invalid-output", {1, 1, 1}, 1, true, true, true, true},
   {"alias-cached", "alias-invalid-output", {1, 1, 1}, 1, true, true, true, true},
   {"alias-missing-need-mmap", "alias-invalid-output", {1, 1, 1}, 1, true, true, true, true},
   {"alias-same-va-only", "alias-invalid-output", {1, 1, 1}, 1, true, true, true, true},
   {"alias-unmap-fail", "alias-unmap-failed", {1, 1, 1, 1, 1}, 2, true, true, true, true, true},
   {"alias-free-fail", "alias-unmapped-free-failed", {1, 1, 1, 1, 0, 1}, 2, true, true, true, true},
   {"backing-unmap-fail", "backing-unmap-failed", {1, 1, 1, 1, 1, 0, 1}, 2, true, true, false, true},
   {"backing-free-fail", "backing-unmapped-free-failed", {1, 1, 0, 0, 0, 0, 0, 1}, 1, true, true},
   {"cross-window", "single-window-policy", {1, 1, 1, 1, 1, 0, 1}, 2},
   {"view-mismatch", "view-mismatch", {1, 1, 1, 1, 1, 0, 1}, 2},
   {"backing-guard-fail", "backing-guard-failed", {1}, 0, true, true},
   {"alias-guard-fail", "alias-guard-failed", {1, 1, 1}, 1, true, true, true, true},
};

static bool
alias_check_result(bool accepted)
{
   unsigned index = 0;
   if (!probe.live) {
      while (index < sizeof(alias_fixtures) / sizeof(alias_fixtures[0]) &&
             !cpu_case(alias_fixtures[index].name))
         index++;
   }
   if (index == sizeof(alias_fixtures) / sizeof(alias_fixtures[0]))
      return false;
   bool expected_acceptance = index < 2;
   bool success = accepted == expected_acceptance && alias_probe.reason &&
                  !strcmp(alias_probe.reason, alias_fixtures[index].reason) &&
                  !memcmp(alias_probe.calls, alias_fixtures[index].calls, sizeof(alias_probe.calls)) &&
                  alias_probe.guard_stage == alias_fixtures[index].guard_stage &&
                  alias_probe.cleanup_uncertain == alias_fixtures[index].uncertain &&
                  alias_probe.backing.allocated == alias_fixtures[index].backing_allocated &&
                  alias_probe.view.allocated == alias_fixtures[index].view_allocated &&
                  !!alias_probe.backing.mapping == alias_fixtures[index].backing_mapped &&
                  !!alias_probe.view.mapping == alias_fixtures[index].view_mapped && !alias_probe.active;
   printf("ALIAS_SAFETY expected_rejection=%u checked=%u alloc=%u back_maps=%u alias=%u alias_maps=%u alias_unmaps=%u alias_frees=%u back_unmaps=%u back_frees=%u\n",
          !expected_acceptance, success, alias_probe.calls[ALIAS_BACKING_ALLOC], alias_probe.calls[ALIAS_BACKING_MAP],
          alias_probe.calls[ALIAS_ALLOC], alias_probe.calls[ALIAS_MAP], alias_probe.calls[ALIAS_UNMAP],
          alias_probe.calls[ALIAS_FREE], alias_probe.calls[ALIAS_BACKING_UNMAP], alias_probe.calls[ALIAS_BACKING_FREE]);
   return success;
}
