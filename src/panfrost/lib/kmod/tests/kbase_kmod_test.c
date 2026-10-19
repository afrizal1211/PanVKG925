#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "drm-uapi/mali_kbase_ioctl.h"
#include "kbase_kmod.h"
#include "kbase_sync.h"
#include "pan_kmod.h"

int __wrap_ioctl(int fd, unsigned long request, ...);
void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd,
                  off_t offset);
void *__wrap_mmap64(void *addr, size_t length, int prot, int flags, int fd,
                    off64_t offset);
int __wrap_munmap(void *addr, size_t length);
int __wrap_close(int fd);
int __wrap_open(const char *path, int flags, ...);
int __wrap_open64(const char *path, int flags, ...);
drmVersionPtr __wrap_drmGetVersion(int fd);

_Static_assert(sizeof(struct kbase_ioctl_mem_jit_init) == 24, "JIT init ABI");
_Static_assert(_IOC_NR(KBASE_IOCTL_MEM_JIT_INIT) == 14, "JIT init ioctl");
_Static_assert(sizeof(union kbase_ioctl_cs_tiler_heap_init) == 16, "heap init ABI");
_Static_assert(offsetof(union kbase_ioctl_cs_tiler_heap_init, out.first_chunk_va) == 8, "heap result ABI");
_Static_assert(_IOC_NR(KBASE_IOCTL_CS_TILER_HEAP_INIT) == 48, "heap init ioctl");
_Static_assert(_IOC_NR(KBASE_IOCTL_CS_TILER_HEAP_TERM) == 49, "heap term ioctl");
_Static_assert(sizeof(struct base_mem_aliasing_info) == 24, "alias entry ABI");
_Static_assert(sizeof(union kbase_ioctl_mem_alias) == 32, "alias ABI");
_Static_assert(_IOC_NR(KBASE_IOCTL_MEM_ALIAS) == 21, "alias ioctl");
_Static_assert(sizeof(struct kbase_ioctl_version_check) == 4, "version ABI");
_Static_assert(offsetof(struct kbase_ioctl_version_check, major) == 0, "version major ABI");
_Static_assert(offsetof(struct kbase_ioctl_version_check, minor) == 2, "version minor ABI");
_Static_assert(sizeof(struct kbase_ioctl_set_flags) == 4, "flags ABI");
_Static_assert(sizeof(struct kbase_ioctl_get_gpuprops) == 16, "properties ABI");
_Static_assert(offsetof(struct kbase_ioctl_get_gpuprops, buffer) == 0, "properties buffer ABI");
_Static_assert(offsetof(struct kbase_ioctl_get_gpuprops, size) == 8, "properties size ABI");
_Static_assert(sizeof(union kbase_ioctl_mem_alloc) == 32, "legacy allocation ABI");
_Static_assert(offsetof(union kbase_ioctl_mem_alloc, out.gpu_va) == 8, "legacy allocation result ABI");
_Static_assert(sizeof(union kbase_ioctl_mem_alloc_ex) == 64, "extended allocation ABI");
_Static_assert(offsetof(union kbase_ioctl_mem_alloc_ex, in.fixed_address) == 32, "extended allocation address ABI");
_Static_assert(sizeof(struct kbase_ioctl_mem_free) == 8, "free ABI");
_Static_assert(sizeof(struct kbase_ioctl_mem_exec_init) == 8, "exec init ABI");
_Static_assert(sizeof(struct kbase_ioctl_mem_sync) == 32, "sync ABI");
_Static_assert(sizeof(struct base_kcpu_command) == 24, "KCPU command ABI");
_Static_assert(_IOC_NR(KBASE_IOCTL_VERSION_CHECK_JM) == 0, "JM version ioctl");
_Static_assert(_IOC_NR(KBASE_IOCTL_VERSION_CHECK_CSF) == 52, "CSF version ioctl");
_Static_assert(_IOC_NR(KBASE_IOCTL_MEM_ALLOC) == 5, "legacy alloc ioctl");
_Static_assert(_IOC_NR(KBASE_IOCTL_MEM_ALLOC_EX) == 59, "extended alloc ioctl");
_Static_assert(_IOC_NR(KBASE_IOCTL_MEM_EXEC_INIT) == 38, "exec init ioctl");
_Static_assert(BASE_MEM_MAP_TRACKING_HANDLE == (3ull << 12), "tracking offset");

#define FAKE_FD 123
#define REOPEN_FD 124
#define IMPORT_FD 125
#define PAGE_SIZE 4096

struct fake_mapping {
   void *address;
   size_t size;
   bool live;
   bool allocated;
};

static struct fake_mapping mappings[32];
static unsigned mapping_count;
static unsigned mmap_calls;
static unsigned munmap_calls;
static unsigned close_calls;
static unsigned drm_version_calls;
static unsigned ioctl_calls;
static unsigned exec_init_calls;
static unsigned mem_alloc_calls;
static unsigned mem_free_calls;
static unsigned mem_sync_calls;
static unsigned jit_init_calls;
static unsigned heap_init_calls;
static unsigned heap_term_calls;
static unsigned alias_calls;
static uint64_t heap_context = 0x90000000ull;
static uint64_t heap_first_chunk = 0x90100000ull;
static uint64_t alias_cookie = 0xa0000000ull;
static uint64_t alias_gpu_va = 0xa0000000ull;
static bool fail_jit_init;
static bool fail_heap_term;
static unsigned mem_import_calls;
static uint64_t import_cookie = 0xb0000000ull;
static uint64_t import_output_pages;
static uint64_t import_output_flags = BASE_MEM_NEED_MMAP | BASE_MEM_PROT_GPU_RD |
                                      BASE_MEM_PROT_GPU_WR;
static bool fail_mem_import;
static bool import_zero_pages;
static bool fail_alias_mmap;
static bool fail_alias_unmap;
static uint64_t alias_output_flags;
static uint64_t alias_output_pages;
static uint64_t jit_va_seen;
static uint64_t jit_phys_seen;
static uint8_t jit_allocations_seen;
static uint8_t last_sync_type;
static uint64_t last_sync_address;
static uint64_t last_sync_size;
static unsigned unexpected_calls;
static bool reject_csf;
static bool reject_jm;
static bool fail_set_flags;
static bool fail_tracking_mmap;
static bool fail_bo_mmap;
static bool fail_mem_alloc;
static bool fail_exec_init;
static bool large_props;
static bool malformed_props;
static bool csf;
static unsigned short version_major;
static unsigned short version_minor;
static uint8_t props_blob[256];
static size_t props_size;
static uint64_t next_cookie = BASE_MEM_COOKIE_BASE;
static unsigned allocator_calls;
static unsigned allocator_fail_at;
static unsigned open_calls;
static bool fail_open;
static int close_fds[4];

static void
check_at(bool condition, const char *expression, unsigned line)
{
   if (!condition) {
      fprintf(stderr, "kbase mock test failure at line %u: %s\n", line, expression);
      abort();
   }
}

#define check(condition) check_at((condition), #condition, __LINE__)

static void
put_le(uint8_t *dst, uint64_t value, unsigned size)
{
   for (unsigned i = 0; i < size; i++)
      dst[i] = value >> (i * 8);
}

static size_t
append_prop(uint8_t *buf, size_t offset, uint32_t key, unsigned size, uint64_t value)
{
   uint32_t header = (key << 2) | (size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3);
   put_le(buf + offset, header, 4);
   put_le(buf + offset + 4, value, size);
   return offset + 4 + size;
}

static void
make_props(bool raw_gpu_id, uint64_t va_bits)
{
   memset(props_blob, 0, sizeof(props_blob));
   props_size = 0;
   if (raw_gpu_id)
      props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_RAW_GPU_ID, 8,
                               (55ull << 32) | (1ull << 16) | 2);
   else {
      props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_PRODUCT_ID, 2, 1);
      props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_MAJOR_REVISION, 1, 2);
      props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_MINOR_REVISION, 1, 3);
      props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_VERSION_STATUS, 1, 4);
   }
   props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_RAW_SHADER_PRESENT, 8, 5);
   props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_RAW_MMU_FEATURES, 4, va_bits);
   props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_RAW_GPU_FEATURES, 8, 0x1122334455667788ull);
   props_size = append_prop(props_blob, props_size, KBASE_GPUPROP_RAW_L2_FEATURES, 4, 0xaabbccdd);
   props_size = append_prop(props_blob, props_size, 100, 2, 0xabcd);
}

static void
reset_fake(void)
{
   for (unsigned i = 0; i < mapping_count; i++) {
      if (mappings[i].live) {
         if (mappings[i].allocated)
            free(mappings[i].address);
         mappings[i].live = false;
      }
   }
   memset(mappings, 0, sizeof(mappings));
   mapping_count = mmap_calls = munmap_calls = close_calls = drm_version_calls = 0;
   ioctl_calls = exec_init_calls = mem_alloc_calls = mem_free_calls = 0;
   mem_sync_calls = last_sync_type = 0;
   jit_init_calls = heap_init_calls = heap_term_calls = alias_calls = 0;
   heap_context = 0x90000000ull;
   heap_first_chunk = 0x90100000ull;
   alias_cookie = alias_gpu_va = 0xa0000000ull;
   fail_jit_init = fail_heap_term = fail_alias_mmap = fail_alias_unmap = false;
   alias_output_flags = BASE_MEM_NEED_MMAP | BASE_MEM_PROT_CPU_RD |
                        BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
   alias_output_pages = 0;
   jit_va_seen = jit_phys_seen = 0;
   jit_allocations_seen = 0;
   last_sync_address = last_sync_size = 0;
   unexpected_calls = 0;
   reject_csf = reject_jm = fail_set_flags = fail_tracking_mmap = false;
   fail_bo_mmap = fail_exec_init = fail_mem_alloc = large_props = malformed_props = false;
   csf = true;
   version_major = 1;
   version_minor = 9;
   mem_import_calls = 0;
   import_cookie = 0xb0000000ull;
   import_output_pages = 0;
   import_output_flags = BASE_MEM_NEED_MMAP | BASE_MEM_PROT_GPU_RD |
                         BASE_MEM_PROT_GPU_WR;
   fail_mem_import = false;
   import_zero_pages = false;
   next_cookie = BASE_MEM_COOKIE_BASE;
   allocator_calls = allocator_fail_at = 0;
   open_calls = 0; fail_open = false; memset(close_fds, 0, sizeof(close_fds));
   make_props(true, 40);
}

int
__wrap_ioctl(int fd, unsigned long request, ...)
{
   void *arg;
   va_list ap;
   va_start(ap, request);
   arg = va_arg(ap, void *);
   va_end(ap);
   check(fd == FAKE_FD || fd == REOPEN_FD);
   ioctl_calls++;

   if (request == KBASE_IOCTL_VERSION_CHECK_CSF) {
      check(fd == REOPEN_FD || ioctl_calls == 1);
      if (reject_csf) {
         errno = ENOTTY;
         return -1;
      }
      struct kbase_ioctl_version_check *ver = arg;
      check(ver->major == 0 && ver->minor == 0);
      ver->major = version_major;
      ver->minor = version_minor;
      return 0;
   }
   if (request == KBASE_IOCTL_VERSION_CHECK_JM) {
      check(fd == REOPEN_FD || ioctl_calls == 2);
      if (reject_jm) {
         errno = ENOTTY;
         return -1;
      }
      struct kbase_ioctl_version_check *ver = arg;
      check(ver->major == 0 && ver->minor == 0);
      ver->major = version_major;
      ver->minor = version_minor;
      csf = false;
      return 0;
   }
   if (request == KBASE_IOCTL_SET_FLAGS) {
      struct kbase_ioctl_set_flags *flags = arg;
      check(flags->create_flags == 0);
      if (fail_set_flags) {
         errno = EPERM;
         return -1;
      }
      return 0;
   }
   if (request == KBASE_IOCTL_GET_GPUPROPS) {
      struct kbase_ioctl_get_gpuprops *req = arg;
      check(req->flags == 0);
      if (!req->buffer && req->size == 0)
         return large_props ? (1024 * 1024 + 1) : props_size;
      check(req->size == props_size);
      memcpy((void *)(uintptr_t)req->buffer, props_blob, props_size);
      if (malformed_props)
         return (int)props_size - 1;
      return (int)props_size;
   }
   if (request == KBASE_IOCTL_MEM_JIT_INIT) {
      struct kbase_ioctl_mem_jit_init *req = arg;
      check(req->max_allocations > 0 && req->group_id == 0 && req->trim_level == 0);
      jit_init_calls++;
      jit_va_seen = req->va_pages;
      jit_phys_seen = req->phys_pages;
      jit_allocations_seen = req->max_allocations;
      if (fail_jit_init) {
         errno = ENOMEM;
         return -1;
      }
      return 0;
   }
   if (request == KBASE_IOCTL_CS_TILER_HEAP_INIT) {
      union kbase_ioctl_cs_tiler_heap_init *req = arg;
      check(req->in.chunk_size && req->in.initial_chunks &&
            req->in.initial_chunks <= req->in.max_chunks && req->in.target_in_flight);
      heap_init_calls++;
      req->out.gpu_heap_va = heap_context;
      req->out.first_chunk_va = heap_first_chunk;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_TILER_HEAP_TERM) {
      heap_term_calls++;
      if (fail_heap_term) {
         errno = EIO;
         return -1;
      }
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_ALIAS) {
      union kbase_ioctl_mem_alias *req = arg;
      check(req->in.nents > 0 && req->in.nents <= 4 && req->in.stride > 0);
      uint64_t pages = req->in.stride * req->in.nents;
      const struct base_mem_aliasing_info *entries =
         (const void *)(uintptr_t)req->in.aliasing_info;
      for (unsigned i = 0; i < req->in.nents; i++) {
         check(entries[i].handle == entries[0].handle && entries[i].offset == 0);
         check(entries[i].length == req->in.stride);
      }
      alias_calls++;
      req->out.flags = alias_output_flags;
      req->out.gpu_va = alias_cookie;
      req->out.va_pages = alias_output_pages ? alias_output_pages : pages;
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_EXEC_INIT) {
      exec_init_calls++;
      check(((struct kbase_ioctl_mem_exec_init *)arg)->va_pages != 0);
      if (fail_exec_init) {
         errno = EPERM;
         return -1;
      }
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_ALLOC_EX) {
      union kbase_ioctl_mem_alloc_ex *req = arg;
      mem_alloc_calls++;
      check(csf && version_major == 1 && version_minor >= 9);
      check(req->in.va_pages > 0 && req->in.commit_pages == req->in.va_pages);
      if (fail_mem_alloc) {
         errno = ENOMEM;
         return -1;
      }
      req->out.flags = req->in.flags;
      if (req->in.flags & BASE_MEM_SAME_VA)
         req->out.gpu_va = next_cookie;
      else
         req->out.gpu_va = 0x80000000ull + mem_alloc_calls * PAGE_SIZE;
      next_cookie += PAGE_SIZE;
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_ALLOC) {
      union kbase_ioctl_mem_alloc *req = arg;
      mem_alloc_calls++;
      check(!csf || version_major != 1 || version_minor < 9);
      check(req->in.va_pages > 0 && req->in.commit_pages == req->in.va_pages);
      if (fail_mem_alloc) {
         errno = ENOMEM;
         return -1;
      }
      req->out.flags = req->in.flags;
      if (req->in.flags & BASE_MEM_SAME_VA)
         req->out.gpu_va = next_cookie;
      else
         req->out.gpu_va = 0x80000000ull + mem_alloc_calls * PAGE_SIZE;
      next_cookie += PAGE_SIZE;
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_FREE) {
      check(((struct kbase_ioctl_mem_free *)arg)->gpu_addr != 0);
      mem_free_calls++;
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_IMPORT) {
      union kbase_ioctl_mem_import *req = arg;
      check(req->in.type == BASE_MEM_IMPORT_TYPE_UMM);
      check(req->in.flags & (BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR));
      check(!(req->in.flags & (BASE_MEM_PROT_GPU_EX | BASE_MEM_GROW_ON_GPF)));
      check(req->in.phandle);
      check(*(const int *)(uintptr_t)req->in.phandle == IMPORT_FD);
      mem_import_calls++;
      if (fail_mem_import) {
         errno = ENOMEM;
         return -1;
      }
      req->out.flags = import_output_flags;
      req->out.gpu_va = import_cookie;
      req->out.va_pages = import_zero_pages ? 0 :
                          import_output_pages ? import_output_pages : 1;
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_SYNC) {
      struct kbase_ioctl_mem_sync *req = arg;
      check(req->handle != 0 && req->size > 0);
      check(req->type == BASE_SYNCSET_OP_MSYNC || req->type == BASE_SYNCSET_OP_CSYNC);
      mem_sync_calls++;
      last_sync_type = req->type;
      last_sync_address = req->user_addr;
      last_sync_size = req->size;
      return 0;
   }

   unexpected_calls++;
   fprintf(stderr, "unexpected ioctl 0x%lx\n", request);
   errno = ENOTTY;
   return -1;
}

void *
__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
   check(fd == FAKE_FD || fd == REOPEN_FD);
   mmap_calls++;
   if (offset == BASE_MEM_MAP_TRACKING_HANDLE) {
      check(length == PAGE_SIZE && prot == PROT_NONE && flags == MAP_SHARED);
      if (fail_tracking_mmap) {
         errno = ENOMEM;
         return MAP_FAILED;
      }
   } else {
      check(offset >= 0 && length &&
            (prot == PROT_NONE || prot == PROT_READ || prot == (PROT_READ | PROT_WRITE)) &&
            (flags == MAP_SHARED || flags == (MAP_SHARED | MAP_FIXED_NOREPLACE)));
      if (fail_bo_mmap || fail_alias_mmap) {
         errno = ENOMEM;
         return MAP_FAILED;
      }
   }
   check(mapping_count < ARRAY_SIZE(mappings));
   void *mapped = aligned_alloc(PAGE_SIZE, (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
   if (offset != BASE_MEM_MAP_TRACKING_HANDLE && prot == PROT_READ && length > PAGE_SIZE) {
      if (mapped)
         free(mapped);
      if (fail_alias_mmap) {
         errno = ENOMEM;
         return MAP_FAILED;
      }
      void *alias_mapping = (void *)(uintptr_t)alias_gpu_va;
      mappings[mapping_count++] = (struct fake_mapping){alias_mapping, length, true, false};
      return alias_mapping;
   }
   bool allocated = mapped != NULL;
   if (!mapped) {
      errno = ENOMEM;
      return MAP_FAILED;
   }
   mappings[mapping_count++] = (struct fake_mapping){mapped, length, true, allocated};
   return mapped;
}

void *
__wrap_mmap64(void *addr, size_t length, int prot, int flags, int fd,
                off64_t offset)
{
   return __wrap_mmap(addr, length, prot, flags, fd, offset);
}

int
__wrap_munmap(void *addr, size_t length)
{
   munmap_calls++;
   for (unsigned i = 0; i < mapping_count; i++) {
      if (mappings[i].address == addr && mappings[i].live) {
         check(length == mappings[i].size);
         if (fail_alias_unmap && !mappings[i].allocated) {
            errno = EIO;
            return -1;
         }
         mappings[i].live = false;
         if (mappings[i].allocated)
            free(addr);
         return 0;
      }
   }
   unexpected_calls++;
   errno = EINVAL;
   return -1;
}

int
__wrap_open(const char *path, int flags, ...)
{
   check(strcmp(path, "/proc/self/fd/123") == 0);
   check(flags == (O_RDWR | O_CLOEXEC));
   open_calls++;
   if (fail_open) {
      errno = EMFILE;
      return -1;
   }
   return REOPEN_FD;
}

int
__wrap_open64(const char *path, int flags, ...)
{
   return __wrap_open(path, flags);
}

int
__wrap_close(int fd)
{
   check(fd == FAKE_FD || fd == REOPEN_FD);
   close_calls++;
   check(close_calls <= ARRAY_SIZE(close_fds));
   close_fds[close_calls - 1] = fd;
   return 0;
}

drmVersionPtr
__wrap_drmGetVersion(int fd)
{
   check(fd == FAKE_FD || fd == REOPEN_FD);
   drm_version_calls++;
   return NULL;
}

static void *
test_zalloc(const struct pan_kmod_allocator *allocator, size_t size, bool transient)
{
   allocator_calls++;
   if (allocator_fail_at && allocator_calls == allocator_fail_at)
      return NULL;
   return calloc(1, size);
}

static void
test_free(const struct pan_kmod_allocator *allocator, void *ptr)
{
   free(ptr);
}

static const struct pan_kmod_allocator allocator = {
   .zalloc = test_zalloc,
   .free = test_free,
};

static struct pan_kmod_dev *
create_device(uint32_t flags)
{
   return kbase_kmod_ops.dev_create(FAKE_FD, flags, &allocator);
}

static void
test_parser(void)
{
   struct pan_kmod_dev_props props;
   make_props(true, 40);
   memset(&props, 0x5a, sizeof(props));
   check(kbase_kmod_parse_props(props_blob, props_size, &props));
   check(props.gpu_id == ((55ull << 32) | (1ull << 16) | 2));
   check(props.shader_present == 5 && MMU_FEATURES_VA_BITS(props.mmu_features) == 40);
   check(props.gpu_features == 0x1122334455667788ull);
   check(props.l2_features == 0xaabbccdd);
   check(props.pgsize_bitmap == PAN_PGSIZE_4K);
   check(props.supported_bo_flags == (PAN_KMOD_BO_FLAG_EXECUTABLE |
                                      PAN_KMOD_BO_FLAG_NO_MMAP |
                                      PAN_KMOD_BO_FLAG_WB_MMAP |
                                      PAN_KMOD_BO_FLAG_GPU_UNCACHED |
                                      PAN_KMOD_BO_FLAG_CSF_EVENT));

   make_props(false, 48);
   check(kbase_kmod_parse_props(props_blob, props_size, &props));
   check(props.gpu_id == ((1ull << 16) | (2ull << 12) | (3ull << 4) | 4));

   struct pan_kmod_dev_props before = props;
   for (size_t truncated = 1; truncated <= 8; truncated++) {
      check(!kbase_kmod_parse_props(props_blob, truncated, &props));
      check(errno == EINVAL && memcmp(&props, &before, sizeof(props)) == 0);
   }
   check(!kbase_kmod_parse_props(props_blob, props_size - 1, &props));
   check(errno == EINVAL && memcmp(&props, &before, sizeof(props)) == 0);
   check(!kbase_kmod_parse_props(NULL, props_size, &props) && errno == EINVAL);
   check(!kbase_kmod_parse_props(props_blob, 0, &props) && errno == EINVAL);
   check(!kbase_kmod_parse_props(props_blob, 1024 * 1024 + 1, &props) && errno == EINVAL);

   make_props(true, 31);
   check(!kbase_kmod_parse_props(props_blob, props_size, &props) && errno == EINVAL);
   make_props(true, 49);
   check(!kbase_kmod_parse_props(props_blob, props_size, &props) && errno == EINVAL);
   make_props(true, 40);
   for (size_t offset = 0; offset + 4 < props_size;) {
      uint32_t header = (uint32_t)props_blob[offset] |
                        (uint32_t)props_blob[offset + 1] << 8 |
                        (uint32_t)props_blob[offset + 2] << 16 |
                        (uint32_t)props_blob[offset + 3] << 24;
      unsigned value_size = 1u << (header & 3);
      if ((header >> 2) == KBASE_GPUPROP_RAW_SHADER_PRESENT) {
         memset(props_blob + offset + 4, 0, value_size);
         break;
      }
      offset += 4 + value_size;
   }
   check(!kbase_kmod_parse_props(props_blob, props_size, &props) && errno == EINVAL);

   uint8_t unknown[8];
   size_t unknown_size = append_prop(unknown, 0, 0x12345, 2, 0xabcd);
   make_props(true, 40);
   memcpy(props_blob + props_size, unknown, unknown_size);
   check(kbase_kmod_parse_props(props_blob, props_size + unknown_size, &props));
   check(props.gpu_features == 0x1122334455667788ull);
   check(!kbase_kmod_parse_props(unknown, 3, &props) && errno == EINVAL);
   put_le(unknown, (0x12345u << 2) | 3, 4);
   check(!kbase_kmod_parse_props(unknown, sizeof(unknown), &props) && errno == EINVAL);
}

static void
test_create_destroy(void)
{
   reset_fake();
   reject_csf = true;
   csf = false;
   version_major = 11;
   version_minor = 2;
   struct pan_kmod_dev *dev = create_device(PAN_KMOD_DEV_FLAG_OWNS_FD);
   check(dev != NULL && dev->driver.version.major == 11 && dev->driver.version.minor == 2);
   check(ioctl_calls >= 4 && exec_init_calls == 1 && mmap_calls == 1);
   check(dev->flags & PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL);
   check(pan_kmod_dev_query_user_va_range(dev).start == 0x2000000ull);
   check(pan_kmod_dev_query_user_va_range(dev).size == (1ull << 39) - 0x2000000ull);
   kbase_kmod_ops.dev_destroy(dev);
   check(close_calls == 1 && munmap_calls == 1 && unexpected_calls == 0);

   reset_fake();
   dev = create_device(0);
   check(dev != NULL && exec_init_calls == 0);
   kbase_kmod_ops.dev_destroy(dev);
   check(close_calls == 0 && munmap_calls == 1);

   reset_fake();
   version_major = 1;
   version_minor = 8;
   fail_exec_init = true;
   dev = create_device(0);
   check(dev != NULL && exec_init_calls == 1);
   check(!(dev->props.supported_bo_flags & PAN_KMOD_BO_FLAG_EXECUTABLE));
   kbase_kmod_ops.dev_destroy(dev);

   reset_fake();
   version_major = 2;
   dev = create_device(0);
   check(dev == NULL && ioctl_calls == 1 && mmap_calls == 0);

   reset_fake();
   reject_csf = true;
   reject_jm = true;
   check(create_device(0) == NULL && ioctl_calls == 2 && mmap_calls == 0);

   reset_fake();
   fail_set_flags = true;
   check(create_device(0) == NULL && mmap_calls == 0);

   reset_fake();
   fail_tracking_mmap = true;
   check(create_device(0) == NULL && munmap_calls == 0);

   reset_fake();
   large_props = true;
   check(create_device(0) == NULL && mmap_calls == 1 && munmap_calls == 1);

   reset_fake();
   malformed_props = true;
   check(create_device(0) == NULL && mmap_calls == 1 && munmap_calls == 1);

   reset_fake();
   allocator_fail_at = 1;
   check(create_device(PAN_KMOD_DEV_FLAG_OWNS_FD) == NULL &&
         close_calls == 0 && munmap_calls == 1);

   reset_fake();
   reject_csf = true;
   version_major = 10;
   check(create_device(PAN_KMOD_DEV_FLAG_OWNS_FD) == NULL &&
         close_calls == 0 && mmap_calls == 0);
}

static void
test_reopen(void)
{
   reset_fake();
   struct pan_kmod_dev *physical = create_device(0);
   check(physical && physical->fd == FAKE_FD);
   struct pan_kmod_dev *dev = kbase_kmod_dev_reopen(physical, 0, &allocator);
   check(dev && dev != physical && dev->fd == REOPEN_FD);
   check(open_calls == 1 && (dev->flags & PAN_KMOD_DEV_FLAG_OWNS_FD));
   check(dev->flags & PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL);
   check(!(physical->flags & PAN_KMOD_DEV_FLAG_OWNS_FD));
   kbase_kmod_ops.dev_destroy(dev);
   check(close_calls == 1 && close_fds[0] == REOPEN_FD);
   kbase_kmod_ops.dev_destroy(physical);
   check(close_calls == 1);

   reset_fake();
   physical = create_device(0);
   check(physical);
   fail_open = true;
   check(!kbase_kmod_dev_reopen(physical, 0, &allocator) && errno == EMFILE);
   check(close_calls == 0 && open_calls == 1);
   fail_open = false;
   reject_csf = true; reject_jm = true;
   check(!kbase_kmod_dev_reopen(physical, 0, &allocator));
   check(close_calls == 1 && close_fds[0] == REOPEN_FD);
   kbase_kmod_ops.dev_destroy(physical);
   check(close_calls == 1);
}

static void
test_alloc_map_vm(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = create_device(0);
   check(dev != NULL);
   struct pan_kmod_bo *bo = pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE + 1, 0);
   check(bo && bo->size == PAGE_SIZE * 2);
   check(mem_alloc_calls == 1 && mmap_calls == 2);
   void *cached = pan_kmod_bo_mmap(bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
   check(cached != MAP_FAILED);
   check(cached == pan_kmod_bo_mmap(bo, PROT_READ | PROT_WRITE, MAP_SHARED, cached));
   check(pan_kmod_bo_mmap(bo, PROT_READ, MAP_SHARED, NULL) == MAP_FAILED && errno == ENOTSUP);
   check(pan_kmod_bo_mmap(bo, PROT_NONE, MAP_SHARED, NULL) == MAP_FAILED && errno == ENOTSUP);
   check(pan_kmod_bo_mmap(bo, PROT_READ | PROT_WRITE, MAP_SHARED,
                          (char *)cached + PAGE_SIZE) == MAP_FAILED && errno == ENOTSUP);
   check(pan_kmod_bo_mmap(bo, PROT_READ, MAP_PRIVATE, NULL) == MAP_FAILED && errno == ENOTSUP);
   check(pan_kmod_bo_mmap(bo, PROT_EXEC, MAP_SHARED, NULL) == MAP_FAILED && errno == ENOTSUP);
   struct pan_kmod_bo *no_mmap = pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE,
                                                    PAN_KMOD_BO_FLAG_NO_MMAP);
   check(no_mmap != NULL);
   check(pan_kmod_bo_mmap(no_mmap, PROT_READ | PROT_WRITE, MAP_SHARED, NULL) == MAP_FAILED);
   pan_kmod_bo_put(no_mmap);
   check(munmap_calls == 1);
   check(pan_kmod_bo_munmap(bo, (char *)cached + PAGE_SIZE, PAGE_SIZE) == 0);
   check(pan_kmod_bo_munmap(bo, (char *)cached + bo->size, 1) == -1 && errno == EINVAL);
   check(munmap_calls == 1);
   check(!pan_kmod_bo_wait(bo, 0, false) && errno == ENOTSUP);
   check(pan_kmod_bo_export(bo) == -1 && errno == ENOTSUP);

   struct pan_kmod_vm *vm = pan_kmod_vm_create(dev, PAN_KMOD_VM_FLAG_AUTO_VA, 0, 0);
   check(vm != NULL);
   check(pan_kmod_vm_create(dev, 0, 0, 0) == NULL && errno == ENOTSUP);
   check(pan_kmod_vm_create(dev, PAN_KMOD_VM_FLAG_AUTO_VA, PAGE_SIZE, 0) == NULL && errno == ENOTSUP);
   struct pan_kmod_vm_op map = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {.start = PAN_KMOD_VM_MAP_AUTO_VA, .size = PAGE_SIZE},
      .map = {.bo = bo, .bo_offset = PAGE_SIZE},
   };
   check(pan_kmod_vm_bind(vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &map, 1) == 0);
   check(map.va.start == (uintptr_t)cached + PAGE_SIZE);
   struct pan_kmod_vm_op unmap = {
      .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
      .va = {.start = PAGE_SIZE, .size = PAGE_SIZE},
   };
   check(pan_kmod_vm_bind(vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &unmap, 1) == 0);
   check(pan_kmod_vm_bind(vm, PAN_KMOD_VM_OP_MODE_ASYNC, &unmap, 1) == -1 && errno == ENOTSUP);
   map = (struct pan_kmod_vm_op){
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {.start = PAN_KMOD_VM_MAP_AUTO_VA, .size = PAGE_SIZE},
      .map = {.bo = bo, .bo_offset = 1},
   };
   check(pan_kmod_vm_bind(vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &map, 1) == -1 && errno == EINVAL);
   map.map.bo_offset = PAGE_SIZE;
   map.va.size = 0;
   check(pan_kmod_vm_bind(vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &map, 1) == -1 && errno == EINVAL);
   map.va.size = PAGE_SIZE;
   map.flags = PAN_KMOD_VM_OP_OP_MAP_SPARSE;
   map.va.start = 0x10000;
   check(dev->ops->vm_bind(vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &map, 1) == -1);
   check(unexpected_calls == 0);

   pan_kmod_vm_destroy(vm);
   pan_kmod_bo_put(bo);
   check(mem_free_calls == 0 && munmap_calls == 2);
   kbase_kmod_ops.dev_destroy(dev);
   check(munmap_calls == 3 && unexpected_calls == 0);
}

static void
test_executable_alloc(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = create_device(0);
   check(dev != NULL);
   struct pan_kmod_bo *bo = pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE,
                                               PAN_KMOD_BO_FLAG_EXECUTABLE);
   check(bo != NULL && mem_alloc_calls == 1);
   check(pan_kmod_bo_size(bo) == PAGE_SIZE);
   pan_kmod_bo_put(bo);
   check(mem_free_calls == 1 && munmap_calls == 1);
   kbase_kmod_ops.dev_destroy(dev);

   reset_fake();
   dev = create_device(0);
   check(dev != NULL);
   allocator_fail_at = allocator_calls + 1;
   check(pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE, 0) == NULL && errno == ENOMEM);
   check(mem_alloc_calls == 0 && munmap_calls == 0);
   kbase_kmod_ops.dev_destroy(dev);

   reset_fake();
   fail_mem_alloc = true;
   dev = create_device(0);
   check(dev != NULL);
   check(pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE, 0) == NULL && errno == ENOMEM);
   check(mem_free_calls == 0 && munmap_calls == 0);
   kbase_kmod_ops.dev_destroy(dev);

   reset_fake();
   fail_bo_mmap = true;
   dev = create_device(0);
   check(dev != NULL);
   check(pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE, 0) == NULL && errno == ENOMEM);
   check(mem_alloc_calls == 1 && mem_free_calls == 1 && munmap_calls == 0);
   kbase_kmod_ops.dev_destroy(dev);

   reset_fake();
   dev = create_device(0);
   check(dev != NULL);
   check(pan_kmod_bo_alloc(dev, NULL, UINT64_MAX, 0) == NULL && errno == EINVAL);
   check(pan_kmod_bo_alloc(dev, NULL, 0, 0) == NULL && errno == EINVAL);
   check(pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE, PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT) == NULL && errno == EINVAL);
   kbase_kmod_ops.dev_destroy(dev);
}

static void
test_jit_heap(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = create_device(0);
   check(dev);
   check(kbase_kmod_heap_create(dev, PAGE_SIZE, 1, 2, 1, 0) == NULL && errno == EINVAL);
   check(jit_init_calls == 0 && heap_init_calls == 0);
   check(kbase_kmod_jit_init(dev, 0, 1024, 4) == -1 && errno == EINVAL);
   check(kbase_kmod_jit_init(dev, 4096, 4097, 4) == -1 && errno == EINVAL);
   check(kbase_kmod_jit_init(dev, 4096, 1024, 256) == -1 && errno == EINVAL);
   check(jit_init_calls == 0);
   fail_jit_init = true;
   check(kbase_kmod_jit_init(dev, 4096, 1024, 4) == -1 && errno == ENOMEM);
   check(kbase_kmod_heap_create(dev, PAGE_SIZE, 1, 2, 1, 0) == NULL);
   fail_jit_init = false;
   check(kbase_kmod_jit_init(dev, 4096, 1024, 4) == 0);
   check(jit_init_calls == 2 && jit_va_seen == 4096 && jit_phys_seen == 1024 &&
         jit_allocations_seen == 4);
   check(kbase_kmod_jit_init(dev, 4096, 1024, 4) == 0 && jit_init_calls == 2);
   check(kbase_kmod_jit_init(dev, 4096, 1024, 5) == -1 && errno == EINVAL &&
         jit_init_calls == 2);
   struct kbase_kmod_heap *heap = kbase_kmod_heap_create(dev, PAGE_SIZE, 1, 2, 1, 0);
   check(heap && heap_init_calls == 1);
   check(kbase_kmod_heap_context(heap) == heap_context &&
         kbase_kmod_heap_first_chunk(heap) == heap_first_chunk);
   fail_heap_term = true;
   check(kbase_kmod_heap_destroy(heap) == -1 && errno == EIO);
   fail_heap_term = false;
   check(kbase_kmod_heap_destroy(heap) == 0 && heap_term_calls == 2);
   check(!kbase_kmod_heap_create(dev, PAGE_SIZE + 1, 1, 2, 1, 0));
   check(!kbase_kmod_heap_create(dev, PAGE_SIZE, 3, 2, 1, 0));
   check(!kbase_kmod_heap_create(dev, PAGE_SIZE, 1, 2, 65536, 0));
   check(!kbase_kmod_heap_create(dev, PAGE_SIZE, 1, 2, 1, 256));

   /* kbase hands out heap contexts from a shared pool, spaced by HEAP_CTX_SIZE
    * (32) rounded up to the GPU cache line. Only the first slot in a context is
    * page aligned; a second heap lands on a 64-byte offset. Rejecting that
    * would break every graphics submission after the first. */
   heap_context = 0x90000040ull;
   heap_first_chunk = 0x90100000ull;
   struct kbase_kmod_heap *heap2 =
      kbase_kmod_heap_create(dev, PAGE_SIZE, 1, 2, 1, 0);
   check(heap2 && kbase_kmod_heap_context(heap2) == 0x90000040ull);
   check(kbase_kmod_heap_destroy(heap2) == 0);
   heap_context = 0x90000000ull;
   heap_first_chunk = 0x90100000ull;

   check(heap_init_calls == 2);
   heap_first_chunk++;
   check(!kbase_kmod_heap_create(dev, PAGE_SIZE, 1, 2, 1, 0) && errno == EINVAL);
   check(heap_init_calls == 3 && heap_term_calls == 4);
   heap_first_chunk--;
   check(kbase_kmod_heap_context(NULL) == 0 && kbase_kmod_heap_first_chunk(NULL) == 0);
   kbase_kmod_ops.dev_destroy(dev);
}

static void
test_alias(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = create_device(0);
   check(dev);
   struct pan_kmod_bo *bo = pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE * 4, 0);
   check(bo);
   int32_t refcnt = bo->refcnt;
   struct kbase_kmod_alias *alias = kbase_kmod_alias_create(bo, PAGE_SIZE, 2);
   check(alias && alias_calls == 1 && bo->refcnt == refcnt + 1);
   check(kbase_kmod_alias_gpu_va(alias) == alias_gpu_va);
   check(kbase_kmod_alias_create(bo, PAGE_SIZE + 1, 1) == NULL && errno == EINVAL);
   check(alias_calls == 1 && mem_free_calls == 0);
   pan_kmod_bo_put(bo);
   check(mem_free_calls == 0);
   check(kbase_kmod_alias_destroy(alias) == 0 && mem_free_calls == 0);
   kbase_kmod_ops.dev_destroy(dev);

   reset_fake();
   dev = create_device(0);
   bo = pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE * 4, 0);
   check(bo);
   fail_alias_mmap = true;
   check(!kbase_kmod_alias_create(bo, PAGE_SIZE, 2) && errno == ENOMEM);
   check(mem_free_calls == 1 && bo->refcnt == 1);
   fail_alias_mmap = false;
   alias_gpu_va = 0xfffff000ull;
   check(!kbase_kmod_alias_create(bo, PAGE_SIZE, 2) && errno == ENOSPC);
   check(alias_calls == 17 && bo->refcnt == 1);
   alias_gpu_va = 0xb0000000ull;
   check(!kbase_kmod_alias_cleanup_uncertain(dev));
   alias_output_flags &= ~BASE_MEM_NEED_MMAP;
   check(!kbase_kmod_alias_create(bo, PAGE_SIZE, 2) && errno == EINVAL);
   check(mem_free_calls == 1 && bo->refcnt == 1);
   check(kbase_kmod_alias_cleanup_uncertain(dev));
   alias_output_flags |= BASE_MEM_NEED_MMAP;
   alias_output_pages = 1;
   check(!kbase_kmod_alias_create(bo, PAGE_SIZE, 2) && errno == EINVAL);
   check(mem_free_calls == 1 && bo->refcnt == 1);
   alias_output_pages = 0;
   alias_cookie = BASE_MEM_COOKIE_BASE + 1;
   check(!kbase_kmod_alias_create(bo, PAGE_SIZE, 2) && errno == EINVAL);
   check(mem_free_calls == 1 && bo->refcnt == 1);
   alias_cookie = BASE_MEM_COOKIE_BASE;
   unsigned aliases_before_invalid = alias_calls;
   check(!kbase_kmod_alias_create(bo, PAGE_SIZE, 5) && errno == EINVAL);
   check(!kbase_kmod_alias_create(bo, PAGE_SIZE * 5, 2) && errno == EINVAL);
   check(!kbase_kmod_alias_create(bo, UINT64_MAX - PAGE_SIZE + 1, 2) && errno == EINVAL);
   check(alias_calls == aliases_before_invalid);
   alias = kbase_kmod_alias_create(bo, PAGE_SIZE, 2);
   check(alias && kbase_kmod_alias_gpu_va(alias) == alias_gpu_va);
   check(kbase_kmod_alias_gpu_va(alias) != alias_cookie);
   check(kbase_kmod_alias_destroy(alias) == 0 && bo->refcnt == 1);
   check(mem_free_calls == 1);
   pan_kmod_bo_put(bo);
   kbase_kmod_ops.dev_destroy(dev);

   reset_fake();
   dev = create_device(0);
   bo = pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE * 4, 0);
   check(bo);
   refcnt = bo->refcnt;
   alias_gpu_va = 0xfffff000ull;
   fail_alias_unmap = true;
   check(!kbase_kmod_alias_cleanup_uncertain(dev));
   check(!kbase_kmod_alias_create(bo, PAGE_SIZE, 2) && errno == EIO);
   check(mem_free_calls == 0 && bo->refcnt == refcnt);
   check(kbase_kmod_alias_cleanup_uncertain(dev));
   fail_alias_unmap = false;
   pan_kmod_bo_put(bo);
   kbase_kmod_ops.dev_destroy(dev);
}

static void
test_sync_provider(void)
{
   struct util_sync_provider *sync = kbase_sync_provider_create();
   check(sync != NULL && sync->timeline_signal == NULL && sync->timeline_wait == NULL && sync->query == NULL);
   uint32_t out_handle = 99;
   int out_fd = 99;
   check(sync->create(sync, 0, &out_handle) == -1 && errno == ENOTSUP && out_handle == 0);
   check(sync->handle_to_fd(sync, 1, &out_fd) == -1 && errno == ENOTSUP && out_fd == -1);
   out_handle = 99;
   check(sync->fd_to_handle(sync, FAKE_FD, &out_handle) == -1 && errno == ENOTSUP && out_handle == 0);
   check(sync->destroy(sync, 1) == -1 && errno == ENOTSUP);
   check(sync->import_sync_file(sync, 1, FAKE_FD) == -1 && errno == ENOTSUP);
   check(sync->export_sync_file(sync, 1, &out_fd) == -1 && errno == ENOTSUP);
   check(sync->wait(sync, NULL, 0, 0, 0, NULL) == -1 && errno == ENOTSUP);
   check(sync->reset(sync, NULL, 0) == -1 && errno == ENOTSUP);
   check(sync->signal(sync, NULL, 0) == -1 && errno == ENOTSUP);
   check(sync->transfer(sync, 0, 0, 0, 0, 0) == -1 && errno == ENOTSUP);
   struct util_sync_provider *clone = sync->clone(sync);
   check(clone != NULL && clone != sync);
   clone->finalize(clone);
   sync->finalize(sync);
}

static void
test_cache_sync(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = create_device(0);
   check(dev != NULL);
   struct pan_kmod_bo *bo =
      pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE, PAN_KMOD_BO_FLAG_WB_MMAP);
   check(bo != NULL);
   void *mapped = pan_kmod_bo_mmap(bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
   check(mapped != MAP_FAILED);

   pan_kmod_queue_bo_map_sync(bo, 0, mapped, PAGE_SIZE,
                              PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH);
   check(bo->has_pending_deferred_syncs && mem_sync_calls == 0);
   pan_kmod_flush_bo_map_syncs(dev);
   check(!bo->has_pending_deferred_syncs && mem_sync_calls == 1);
   check(last_sync_type == BASE_SYNCSET_OP_MSYNC &&
         last_sync_address == (uintptr_t)mapped && last_sync_size == PAGE_SIZE);

   pan_kmod_queue_bo_map_sync(bo, 0, mapped, PAGE_SIZE,
                              PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE);
   pan_kmod_bo_put(bo);
   check(mem_sync_calls == 2 && last_sync_type == BASE_SYNCSET_OP_CSYNC);
   check(munmap_calls == 1);
   pan_kmod_dev_destroy(dev);
   check(munmap_calls == 2 && unexpected_calls == 0);

   /* An imported dma-buf BO is NO_MMAP because the kernel rejects CPU faults
    * on it, but it still owns a mapping the exporter can maintain. It must be
    * syncable so a caller can invalidate before reading GPU-written data.
    */
   reset_fake();
   dev = create_device(0);
   check(dev != NULL);
   struct pan_kmod_bo *imported = pan_kmod_bo_import(dev, IMPORT_FD);
   check(imported != NULL);
   check(imported->flags & PAN_KMOD_BO_FLAG_IMPORTED);
   check(imported->flags & PAN_KMOD_BO_FLAG_NO_MMAP);
   unsigned before = mem_sync_calls;
   check(kbase_kmod_bo_sync(imported, 0, PAGE_SIZE,
                            PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE) == 0);
   check(mem_sync_calls == before + 1);
   pan_kmod_bo_put(imported);
   pan_kmod_dev_destroy(dev);

   /* A NO_MMAP BO that was not imported has no mapping at all and must still
    * be refused.
    */
   reset_fake();
   dev = create_device(0);
   check(dev != NULL);
   struct pan_kmod_bo *no_mmap =
      pan_kmod_bo_alloc(dev, NULL, PAGE_SIZE, PAN_KMOD_BO_FLAG_NO_MMAP);
   check(no_mmap != NULL);
   check(kbase_kmod_bo_sync(no_mmap, 0, PAGE_SIZE,
                            PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH) == -1 &&
         errno == EINVAL);
   pan_kmod_bo_put(no_mmap);
   pan_kmod_dev_destroy(dev);
}

static void
test_import(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = create_device(0);
   check(dev != NULL);

   /* A dma-buf import becomes a GPU-visible BO with no CPU mapping. */
   struct pan_kmod_bo *bo = pan_kmod_bo_import(dev, IMPORT_FD);
   check(bo != NULL);
   check(mem_import_calls == 1);
   check(mmap_calls == 2); /* tracking page + cookie */
   check(bo->size == PAGE_SIZE);
   check(bo->flags & PAN_KMOD_BO_FLAG_IMPORTED);
   check(bo->flags & PAN_KMOD_BO_FLAG_NO_MMAP);
   check(pan_kmod_bo_mmap(bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL) == MAP_FAILED);
   pan_kmod_bo_put(bo);
   /* SAME_VA regions are torn down by munmap, never by MEM_FREE. */
   check(munmap_calls == 1);
   check(mem_free_calls == 0);

   /* Import failure leaves nothing behind. */
   reset_fake();
   dev = create_device(0);
   check(dev != NULL);
   fail_mem_import = true;
   check(pan_kmod_bo_import(dev, IMPORT_FD) == NULL);
   check(mem_free_calls == 0);
   check(munmap_calls == 0);

   /* An output that fails validation cannot be trusted, so no MEM_FREE. */
   reset_fake();
   dev = create_device(0);
   check(dev != NULL);
   import_zero_pages = true;
   check(pan_kmod_bo_import(dev, IMPORT_FD) == NULL && errno == EINVAL);
   check(mem_free_calls == 0);

   /* Without NEED_MMAP/SAME_VA the kernel returns a real GPU VA and maps the
    * region itself: no cookie, no placement mmap, and MEM_FREE on destroy.
    */
   reset_fake();
   dev = create_device(0);
   check(dev != NULL);
   import_output_flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
   struct pan_kmod_bo *direct = pan_kmod_bo_import(dev, IMPORT_FD);
   check(direct != NULL);
   check(mem_import_calls == 1);
   check(mmap_calls == 1); /* tracking page only */
   check(direct->size == PAGE_SIZE);
   pan_kmod_bo_put(direct);
   check(mem_free_calls == 1);
   /* No CPU mapping was ever created, so there is nothing to munmap. */
   check(munmap_calls == 0);
}

static void
test_perf_unsupported(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = create_device(0);
   check(dev != NULL);
   check(!pan_kmod_perf_create(dev));
   check(errno == ENOTSUP);

   /* The kbase backend leaves perf_enable/disable/dump/destroy unset, so the
    * core wrappers must refuse instead of dereferencing a NULL op.
    */
   struct pan_kmod_perf_session session = {.dev = dev};
   struct mali_perf_dump_info info;
   check(pan_kmod_perf_enable(&session, NULL) == -1 && errno == ENOTSUP);
   check(pan_kmod_perf_disable(&session) == -1 && errno == ENOTSUP);
   pan_kmod_perf_dump(&session, &info);
   pan_kmod_perf_destroy(&session);
}

static void
test_g925_model(void)
{
   uint8_t blob[128];
   size_t size = 0;
   size = append_prop(blob, size, KBASE_GPUPROP_RAW_GPU_ID, 4, 0xd8300000);
   size = append_prop(blob, size, KBASE_GPUPROP_RAW_CORE_FEATURES, 4, 4);
   size = append_prop(blob, size, KBASE_GPUPROP_RAW_SHADER_PRESENT, 8, 0xfff);
   size = append_prop(blob, size, KBASE_GPUPROP_RAW_MMU_FEATURES, 4, 48);
   size = append_prop(blob, size, KBASE_GPUPROP_RAW_GPU_FEATURES, 8, 1ull << 2);
   struct pan_kmod_dev_props props;
   check(kbase_kmod_parse_props(blob, size, &props));
   const struct pan_model *model =
      pan_get_model(props.gpu_id, props.gpu_variant,
                    util_bitcount64(props.shader_present), props.gpu_features);
   check(model && strcmp(model->name, "Mali-G925-Immortalis") == 0);
   reset_fake();
   version_minor = 30;
   struct pan_kmod_dev *dev = create_device(0);
   check(dev);
   struct pan_kmod_dev_props saved = dev->props;
   dev->props = props;
   check(kbase_kmod_supports_panvk(dev));
   dev->props.gpu_variant = 3;
   check(!kbase_kmod_supports_panvk(dev));
   dev->props = props;
   dev->props.shader_present = 0x1ff;
   check(!kbase_kmod_supports_panvk(dev));
   dev->props = props;
   dev->props.gpu_features = 0;
   check(!kbase_kmod_supports_panvk(dev));
   dev->props = props;
   dev->props.mmu_features = 40;
   check(!kbase_kmod_supports_panvk(dev));
   dev->props = props;
   dev->driver.version.minor = 31;
   check(!kbase_kmod_supports_panvk(dev));
   dev->driver.version.minor = 30;
   dev->props = saved;
   check(!kbase_kmod_supports_panvk(dev));
   kbase_kmod_ops.dev_destroy(dev);
   model = pan_get_model(props.gpu_id, props.gpu_variant, 9, props.gpu_features);
   check(model && strcmp(model->name, "Mali-G725") == 0);
   model = pan_get_model(props.gpu_id, props.gpu_variant, 12, 0);
   check(model && strcmp(model->name, "Mali-G725") == 0);
}

static void
test_selection(const char *mode)
{
   reset_fake();
   if (!strcmp(mode, "--default-probe")) {
      setenv("PAN_KMOD_RESTRICT_TO_BACKENDS", "all", 1);
      check(pan_kmod_dev_create(FAKE_FD, 0, &allocator) == NULL);
      check(ioctl_calls == 0 && mmap_calls == 0 && close_calls == 0);
   } else if (!strcmp(mode, "--explicit-probe")) {
      setenv("PAN_KMOD_RESTRICT_TO_BACKENDS", "kbase", 1);
      struct pan_kmod_dev *dev = pan_kmod_dev_create(FAKE_FD, 0, &allocator);
      check(dev != NULL && ioctl_calls > 0);
      pan_kmod_dev_destroy(dev);
   } else {
      check(false);
   }
}

int
main(int argc, char **argv)
{
   reset_fake();
   if (argc == 2) {
      test_selection(argv[1]);
   } else {
      test_parser();
      test_create_destroy();
      test_reopen();
      test_alloc_map_vm();
      test_executable_alloc();
      test_jit_heap();
      test_alias();
      test_sync_provider();
      test_cache_sync();
      test_import();
      test_perf_unsupported();
      test_g925_model();
   }
   check(unexpected_calls == 0);
   for (unsigned i = 0; i < mapping_count; i++)
      check(!mappings[i].live);
   return 0;
}
