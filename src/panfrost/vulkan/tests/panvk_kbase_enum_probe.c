#include "panvk_kbase_icd_guard.h"
#include "drm-uapi/mali_kbase_ioctl.h"
#include "util/mesa-blake3.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define PROPS_LIMIT (1024 * 1024)
#define CSF_MAX_GROUPS 256u
#define CSF_MAX_STREAMS 65536u
#define MEMORY_FLAGS (BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | \
                      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | \
                      BASE_MEM_COHERENT_LOCAL | BASE_MEM_CACHED_CPU | BASE_MEM_SAME_VA)
#define EVENT_FLAGS (BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | \
                     BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR | \
                     BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT)

_Static_assert(MEMORY_FLAGS == 0x380f, "ordinary memory flags");
_Static_assert(EVENT_FLAGS == 0x8200f, "event memory flags");

_Static_assert(sizeof(union kbase_ioctl_mem_alloc_ex) == 64, "allocation ABI");
_Static_assert(sizeof(struct kbase_ioctl_mem_sync) == 32, "cache sync ABI");
_Static_assert(sizeof(struct kbase_ioctl_mem_free) == 8, "memory free ABI");
_Static_assert(offsetof(union kbase_ioctl_mem_alloc_ex, in.flags) == 24, "allocation flags ABI");
_Static_assert(offsetof(struct kbase_ioctl_mem_sync, type) == 24, "cache sync type ABI");

_Static_assert(sizeof(union kbase_ioctl_cs_get_glb_iface) == 24, "CSF query ABI");
_Static_assert(sizeof(struct basep_cs_group_control) == 16, "CSF group ABI");
_Static_assert(sizeof(struct basep_cs_stream_control) == 8, "CSF stream ABI");
_Static_assert(offsetof(union kbase_ioctl_cs_get_glb_iface, in.groups_ptr) == 8, "CSF group pointer ABI");
_Static_assert(offsetof(union kbase_ioctl_cs_get_glb_iface, out.total_stream_num) == 16, "CSF stream count ABI");

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

static struct {
   bool active;
   bool live;
   bool handed_off;
   bool closed;
   int fd;
   unsigned version_calls;
   unsigned flags_calls;
   unsigned props_calls;
   unsigned maps;
   unsigned unmaps;
   unsigned closes;
   unsigned blocked;
   unsigned size;
   uint16_t major;
   uint16_t minor;
   void *tracking;
   uint8_t properties[PROPS_LIMIT];
   bool csf_mode;
   bool csf_query_active;
   unsigned csf_calls;
   int csf_results[2];
   int csf_errors[2];
   union kbase_ioctl_cs_get_glb_iface csf_phases[2];
   uint32_t csf_groups;
   uint32_t csf_streams;
   struct basep_cs_group_control *groups;
   struct basep_cs_stream_control *streams;
   const char *cpu_case;
   const char *csf_reason;
   bool memory_mode;
   bool event_mode;
   bool alias_mode;
   bool device_mode;
   bool submit_mode;
   int reopen_fd;
   bool reopen_delivered;
   bool reopen_closed;
   unsigned device_opens;
   unsigned device_closes;
   bool memory_active;
   bool memory_allocated;
   bool memory_output_valid;
   bool memory_cleanup_uncertain;
   bool memory_cookie_guard;
   uint64_t memory_cookie;
   uint64_t memory_output_flags;
   void *memory_mapping;
   unsigned memory_alloc_calls;
   unsigned memory_map_calls;
   unsigned memory_unmap_calls;
   unsigned memory_free_calls;
   unsigned memory_sync_calls;
   int memory_results[6];
   int memory_errors[6];
   unsigned memory_operation_mask;
   const char *memory_reason;
} probe;

static int (*host_open)(const char *, int, ...);
static int (*host_openat)(int, const char *, int, ...);
static int (*host_ioctl)(int, unsigned long, ...);
static int (*host_close)(int);
static void *(*host_mmap)(void *, size_t, int, int, int, off_t);
static int (*host_munmap)(void *, size_t);
static ssize_t (*host_read)(int, void *, size_t);
static int (*host_ppoll)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);

int open(const char *, int, ...);
int open64(const char *, int, ...);
int openat(int, const char *, int, ...);
int openat64(int, const char *, int, ...);
int ioctl(int, unsigned long, ...);
int close(int);
void *mmap(void *, size_t, int, int, int, off_t);
void *mmap64(void *, size_t, int, int, int, off64_t);
int munmap(void *, size_t);
ssize_t read(int, void *, size_t);
int ppoll(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);

static void
resolve_host(void)
{
   host_open = dlsym(RTLD_NEXT, "open64");
   host_openat = dlsym(RTLD_NEXT, "openat64");
   host_ioctl = dlsym(RTLD_NEXT, "ioctl");
   host_close = dlsym(RTLD_NEXT, "close");
   host_mmap = dlsym(RTLD_NEXT, "mmap64");
   host_munmap = dlsym(RTLD_NEXT, "munmap");
   host_read = dlsym(RTLD_NEXT, "read");
   host_ppoll = dlsym(RTLD_NEXT, "ppoll");
   CHECK(host_open && host_openat && host_ioctl && host_close && host_mmap && host_munmap && host_read && host_ppoll);
}

static int
blocked(void)
{
   probe.blocked++;
   errno = EPERM;
   return -1;
}

static int
open_selected(const char *path, int flags, mode_t mode)
{
   if (!probe.active)
      return host_open(path, flags, mode);
   if (probe.device_mode) {
      if (!strcmp(path, "/dev/mali0")) {
         if (flags != (O_RDWR | O_CLOEXEC) || probe.handed_off || probe.closed)
            return blocked();
         probe.handed_off = true;
         return probe.fd;
      }
      /* The guard keeps denying every open, so the single logical-device reopen
       * is served by the descriptor the child opened before installing it. */
      char expected[64];
      snprintf(expected, sizeof(expected), "/proc/self/fd/%d", probe.fd);
      if (!strcmp(path, expected)) {
         if (probe.reopen_fd <= 0 || probe.reopen_delivered ||
             flags != (O_RDWR | O_CLOEXEC))
            return blocked();
         probe.reopen_delivered = true;
         probe.device_opens++;
         return probe.reopen_fd;
      }
      if (!strncmp(path, "/dev/", 5) || !strncmp(path, "/proc/self/fd/", 14))
         return blocked();
      errno = EPERM;
      return -1;
   }
   if (strcmp(path, "/dev/mali0")) {
      if (!strncmp(path, "/dev/", 5) || !strncmp(path, "/proc/self/fd/", 14))
         return blocked();
      errno = EPERM;
      return -1;
   }
   if (flags != (O_RDWR | O_CLOEXEC) || probe.handed_off || probe.closed)
      return blocked();
   probe.handed_off = true;
   return probe.fd;
}

int
open(const char *path, int flags, ...)
{
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
   }
   return open_selected(path, flags, mode);
}

int
open64(const char *path, int flags, ...)
{
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
   }
   return open_selected(path, flags, mode);
}

int
openat(int dirfd, const char *path, int flags, ...)
{
   if (probe.active) {
      errno = EPERM;
      return -1;
   }
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
   }
   return host_openat(dirfd, path, flags, mode);
}

int
openat64(int dirfd, const char *path, int flags, ...)
{
   if (probe.active) {
      errno = EPERM;
      return -1;
   }
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
   }
   return host_openat(dirfd, path, flags, mode);
}

static size_t
append_property(uint8_t *buffer, size_t offset, unsigned key, uint64_t value, unsigned bytes)
{
   unsigned code = bytes == 8 ? 3 : bytes == 4 ? 2 : bytes == 2 ? 1 : 0;
   uint32_t header = (key << 2) | code;
   memcpy(buffer + offset, &header, sizeof(header));
   memcpy(buffer + offset + sizeof(header), &value, bytes);
   return offset + sizeof(header) + bytes;
}

static unsigned
synthetic_properties(uint8_t *data)
{
   size_t offset = 0;
#define PROP(key, val, bytes) offset = append_property(data, offset, key, val, bytes)
   PROP(KBASE_GPUPROP_RAW_GPU_ID, 0xd8300000, 4);
   PROP(KBASE_GPUPROP_RAW_CORE_FEATURES, 4, 4);
   PROP(KBASE_GPUPROP_RAW_GPU_FEATURES, 4, 8);
   PROP(KBASE_GPUPROP_RAW_SHADER_PRESENT, 0xfff, 8);
   PROP(KBASE_GPUPROP_RAW_MMU_FEATURES, 48, 4);
   PROP(KBASE_GPUPROP_RAW_TILER_FEATURES, (8 << 8) | 9, 4);
   PROP(KBASE_GPUPROP_RAW_THREAD_MAX_THREADS, 2048, 4);
   PROP(KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE, 1024, 4);
   PROP(KBASE_GPUPROP_RAW_THREAD_FEATURES, (16u << 24) | 32768, 4);
   PROP(KBASE_GPUPROP_TLS_ALLOC, 2048, 4);
   PROP(KBASE_GPUPROP_RAW_L2_FEATURES, 0x06100606, 4);
#undef PROP
   return offset;
}

static bool
cpu_case(const char *name)
{
   return probe.cpu_case && !strcmp(probe.cpu_case, name);
}

static int
query_csf_ioctl(void *arg)
{
   if (!probe.csf_mode || !probe.csf_query_active || probe.version_calls != 1 ||
       probe.major != 1 || probe.minor != 30 || probe.flags_calls != 1 ||
       probe.props_calls != 2 || probe.maps != 1 || probe.unmaps || probe.closes ||
       probe.csf_calls >= 2)
      return blocked();
   union kbase_ioctl_cs_get_glb_iface *r = arg;
   union kbase_ioctl_cs_get_glb_iface input = *r;
   unsigned phase = probe.csf_calls;
   if (!phase) {
      const union kbase_ioctl_cs_get_glb_iface empty = {0};
      if (memcmp(r, &empty, sizeof(empty)))
         return blocked();
   } else if (input.in.max_group_num != probe.csf_groups ||
              input.in.max_total_stream_num != probe.csf_streams ||
              input.in.groups_ptr != (uintptr_t)probe.groups ||
              input.in.streams_ptr != (uintptr_t)probe.streams ||
              !probe.csf_groups || probe.csf_groups > CSF_MAX_GROUPS ||
              !probe.csf_streams || probe.csf_streams > CSF_MAX_STREAMS)
      return blocked();
   probe.csf_calls++;
   int result;
   if (probe.live) {
      result = host_ioctl(probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, r);
   } else {
      if (phase) {
         for (unsigned i = 0; i < probe.csf_groups; i++)
            probe.groups[i] = (struct basep_cs_group_control){
               .features = 0x100 + i, .stream_num = i ? 2 : 1,
               .suspend_size = 4096 * (i + 1),
            };
         for (unsigned i = 0; i < probe.csf_streams; i++)
            probe.streams[i] = (struct basep_cs_stream_control){.features = 0x800};
         if (cpu_case("bad-sum"))
            probe.groups[0].stream_num++;
         if (cpu_case("nonuniform"))
            probe.streams[probe.csf_streams - 1].features |= 1u << 20;
         if (cpu_case("scoreboard"))
            for (unsigned i = 0; i < probe.csf_streams; i++)
               probe.streams[i].features = 0x300;
      }
      *r = (union kbase_ioctl_cs_get_glb_iface){0};
      r->out.glb_version = 0x1001e;
      r->out.features = 0x1234;
      r->out.group_num = 2;
      r->out.prfcnt_size = 1024;
      r->out.total_stream_num = 3;
      r->out.instr_features = 0x5678;
      if (phase && cpu_case("count-change"))
         r->out.group_num++;
      result = 0;
   }
   int error = result ? errno : 0;
   probe.csf_results[phase] = result;
   probe.csf_errors[phase] = error;
   probe.csf_phases[phase] = *r;
   fprintf(stderr, "CS_GET_GLB_IFACE phase=%u ret=%d errno=%d\n", phase + 1, result, error);
   if (result)
      errno = error;
   return result;
}

static void
memory_result(unsigned operation, int result)
{
   CHECK(operation < 6);
   probe.memory_results[operation] = result;
   probe.memory_errors[operation] = result ? errno : 0;
   probe.memory_operation_mask |= 1u << operation;
}

static bool
valid_memory_cookie(uint64_t cookie)
{
   return cookie >= BASE_MEM_COOKIE_BASE && cookie < BASE_MEM_FIRST_FREE_ADDRESS &&
          !(cookie & 4095);
}

static uint64_t
memory_input_flags(void)
{
   return probe.event_mode ? EVENT_FLAGS : MEMORY_FLAGS;
}

static int
memory_ioctl(unsigned long request, void *arg)
{
   if (!probe.memory_mode || !probe.memory_active || probe.csf_mode ||
       probe.props_calls != 2 || probe.maps != 1 || probe.unmaps || probe.closes)
      return blocked();
   if (request == KBASE_IOCTL_MEM_ALLOC_EX) {
      union kbase_ioctl_mem_alloc_ex *r = arg;
      const union kbase_ioctl_mem_alloc_ex expected = {
         .in = {.va_pages = 1, .commit_pages = 1, .flags = memory_input_flags()},
      };
      if (probe.memory_alloc_calls || memcmp(r, &expected, sizeof(expected)))
         return blocked();
      probe.memory_alloc_calls++;
      int result;
      if (probe.live)
         result = host_ioctl(probe.fd, request, r);
      else if (cpu_case("allocation-fail")) {
         errno = ENOMEM;
         result = -1;
      } else {
         r->out.flags = memory_input_flags();
         if (probe.event_mode)
            r->out.flags |= BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM;
         if (cpu_case("invalid-flags"))
            r->out.flags |= BASE_MEM_PROT_GPU_EX;
         if (cpu_case("missing-event"))
            r->out.flags &= ~BASE_MEM_CSF_EVENT;
         r->out.gpu_va = cpu_case("invalid-output") ? 0x1003 : BASE_MEM_COOKIE_BASE;
         result = 0;
      }
      memory_result(0, result);
      if (!result) {
         probe.memory_allocated = true;
         probe.memory_cookie = r->out.gpu_va;
         probe.memory_output_flags = r->out.flags;
      }
      printf("MEM_ALLOC_EX request=0x%lx ret=%d errno=%d flags_in=0x%llx flags_out=0x%llx cookie=0x%llx output_authoritative=%u\n",
             request, result, result ? errno : 0, (unsigned long long)memory_input_flags(),
             (unsigned long long)r->out.flags, (unsigned long long)r->out.gpu_va, !result);
      return result;
   }
   if (request == KBASE_IOCTL_MEM_SYNC) {
      struct kbase_ioctl_mem_sync *r = arg;
      struct kbase_ioctl_mem_sync expected = {
         .handle = (uintptr_t)probe.memory_mapping,
         .user_addr = (uintptr_t)probe.memory_mapping,
         .size = 4096,
         .type = probe.memory_sync_calls ? BASE_SYNCSET_OP_CSYNC : BASE_SYNCSET_OP_MSYNC,
      };
      if (!probe.memory_mapping || !probe.memory_output_valid ||
          probe.memory_sync_calls >= 2 || memcmp(r, &expected, sizeof(expected)))
         return blocked();
      unsigned operation = 2 + probe.memory_sync_calls++;
      int result;
      if (probe.live)
         result = host_ioctl(probe.fd, request, r);
      else if ((operation == 2 && cpu_case("clean-fail")) ||
               (operation == 3 && cpu_case("invalidate-fail"))) {
         errno = EIO;
         result = -1;
      } else
         result = 0;
      memory_result(operation, result);
      printf("MEM_SYNC type=%u ret=%d errno=%d handle=0x%llx user_addr=0x%llx bytes=%llu\n",
             r->type, result, result ? errno : 0, (unsigned long long)r->handle,
             (unsigned long long)r->user_addr, (unsigned long long)r->size);
      return result;
   }
   if (request == KBASE_IOCTL_MEM_FREE) {
      struct kbase_ioctl_mem_free *r = arg;
      if (!probe.memory_allocated || !probe.memory_output_valid || probe.memory_mapping ||
          (probe.memory_map_calls != 1 && (probe.memory_cookie_guard || probe.event_mode)) || probe.memory_free_calls ||
          r->gpu_addr != probe.memory_cookie || !valid_memory_cookie(r->gpu_addr))
         return blocked();
      probe.memory_free_calls++;
      int result;
      if (probe.live)
         result = host_ioctl(probe.fd, request, r);
      else if (cpu_case("free-fail")) {
         errno = EIO;
         result = -1;
      } else
         result = 0;
      memory_result(5, result);
      if (!result)
         probe.memory_allocated = false;
      else
         probe.memory_cleanup_uncertain = true;
      printf("MEM_FREE unmapped_cookie=0x%llx ret=%d errno=%d\n",
             (unsigned long long)r->gpu_addr, result, result ? errno : 0);
      return result;
   }
   return blocked();
}

#include "panvk_kbase_alias_probe.inc"
#include "panvk_kbase_device_probe.inc"

int
ioctl(int fd, unsigned long request, ...)
{
   va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
   if (!probe.active || !probe.handed_off || !arg)
      return blocked();
   if (probe.device_mode) {
      if (fd != probe.fd && fd != probe.reopen_fd)
         return blocked();
      return device_ioctl(fd, request, arg);
   }
   if (fd != probe.fd || probe.closed)
      return blocked();
   if (request == KBASE_IOCTL_VERSION_CHECK_CSF) {
      struct kbase_ioctl_version_check *r = arg;
      if (probe.version_calls || probe.flags_calls || r->major || r->minor)
         return blocked();
      probe.version_calls++;
      int result = 0;
      if (probe.live)
         result = host_ioctl(fd, request, r);
      else {
         r->major = 1;
         r->minor = 30;
      }
      probe.major = r->major;
      probe.minor = r->minor;
      fprintf(stderr, "VERSION_CHECK_CSF ret=%d version=%u.%u errno=%d\n", result, r->major, r->minor, result ? errno : 0);
      return result;
   }
   if (request == KBASE_IOCTL_SET_FLAGS) {
      const struct kbase_ioctl_set_flags *r = arg;
      if (probe.version_calls != 1 || probe.major != 1 || probe.minor != 30 ||
          probe.flags_calls || r->create_flags)
         return blocked();
      probe.flags_calls++;
      int result = probe.live ? host_ioctl(fd, request, arg) : 0;
      fprintf(stderr, "SET_FLAGS ret=%d flags=0 errno=%d\n", result, result ? errno : 0);
      return result;
   }
   if (request == KBASE_IOCTL_GET_GPUPROPS) {
      struct kbase_ioctl_get_gpuprops *r = arg;
      if (probe.flags_calls != 1 || probe.maps != 1 || r->flags || probe.props_calls >= 2)
         return blocked();
      if ((!probe.props_calls && (r->buffer || r->size)) ||
          (probe.props_calls && (!r->buffer || r->size != probe.size || !r->size || r->size > PROPS_LIMIT)))
         return blocked();
      probe.props_calls++;
      int result;
      if (probe.live)
         result = host_ioctl(fd, request, arg);
      else if (probe.props_calls == 1)
         result = synthetic_properties(probe.properties);
      else {
         memcpy((void *)(uintptr_t)r->buffer, probe.properties, probe.size);
         result = probe.size;
      }
      fprintf(stderr, "GET_GPUPROPS phase=%u ret=%d errno=%d\n", probe.props_calls, result, result < 0 ? errno : 0);
      if (probe.props_calls == 1) {
         if (result > 0 && result <= PROPS_LIMIT)
            probe.size = result;
      } else if (result >= 0 && result <= probe.size) {
         unsigned actual = result ? result : probe.size;
         memcpy(probe.properties, (void *)(uintptr_t)r->buffer, actual);
         probe.size = actual;
      }
      return result;
   }
   if (probe.alias_mode && (request == KBASE_IOCTL_MEM_ALLOC_EX ||
                            request == KBASE_IOCTL_MEM_ALIAS || request == KBASE_IOCTL_MEM_FREE))
      return alias_ioctl(request, arg);
   if (request == KBASE_IOCTL_CS_GET_GLB_IFACE)
      return query_csf_ioctl(arg);
   if (request == KBASE_IOCTL_MEM_ALLOC_EX || request == KBASE_IOCTL_MEM_SYNC ||
       request == KBASE_IOCTL_MEM_FREE)
      return memory_ioctl(request, arg);
   fprintf(stderr, "Blocked ioctl request=0x%lx nr=%u\n", request, (unsigned)_IOC_NR(request));
   return blocked();
}

void *
mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
   if (!probe.active)
      return host_mmap(address, size, prot, flags, fd, offset);
   if (fd == -1 && (flags & MAP_ANONYMOUS))
      return host_mmap(address, size, prot, flags, fd, offset);
   if (probe.device_mode) {
      if (fd != probe.fd && fd != probe.reopen_fd) {
         blocked();
         return MAP_FAILED;
      }
      return device_mmap(address, size, prot, flags, fd, offset);
   }
   if (probe.alias_mode && alias_probe.active)
      return alias_mmap(address, size, prot, flags, fd, offset);
   if (probe.memory_mode && probe.memory_active &&
       offset == (off_t)probe.memory_cookie && probe.memory_allocated) {
      if (!probe.memory_output_valid || !probe.memory_cookie_guard ||
          probe.memory_map_calls || probe.memory_mapping || probe.closed ||
          fd != probe.fd || address || size != 4096 ||
          prot != (PROT_READ | PROT_WRITE) || flags != MAP_SHARED) {
         blocked();
         return MAP_FAILED;
      }
      probe.memory_map_calls++;
      void *ptr;
      if (probe.live)
         ptr = host_mmap(NULL, size, prot, flags, fd, offset);
      else if (cpu_case("mapping-fail") || cpu_case("free-fail")) {
         errno = ENOMEM;
         ptr = MAP_FAILED;
      } else
         ptr = host_mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      memory_result(1, ptr == MAP_FAILED ? -1 : 0);
      if (ptr != MAP_FAILED)
         probe.memory_mapping = ptr;
      printf("MEM_MAP bytes=%zu cookie=0x%llx cpu_gpu_va=0x%llx ret=%s errno=%d\n",
             size, (unsigned long long)probe.memory_cookie, (unsigned long long)(uintptr_t)ptr,
             ptr == MAP_FAILED ? "failed" : "mapped", ptr == MAP_FAILED ? errno : 0);
      return ptr;
   }
   if (fd != probe.fd || probe.closed || probe.flags_calls != 1 || probe.maps ||
       address || size != 4096 || prot != PROT_NONE || flags != MAP_SHARED ||
       offset != BASE_MEM_MAP_TRACKING_HANDLE) {
      blocked();
      return MAP_FAILED;
   }
   void *ptr = probe.live ? host_mmap(NULL, size, prot, flags, fd, offset) :
      host_mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (ptr != MAP_FAILED) {
      probe.maps++;
      probe.tracking = ptr;
   }
   fprintf(stderr, "tracking mmap length=%zu offset=0x%llx result=%s errno=%d\n",
           size, (unsigned long long)offset, ptr == MAP_FAILED ? "failed" : "mapped", ptr == MAP_FAILED ? errno : 0);
   return ptr;
}

void *
mmap64(void *address, size_t size, int prot, int flags, int fd, off64_t offset)
{
   return mmap(address, size, prot, flags, fd, offset);
}

int
munmap(void *address, size_t size)
{
   if (probe.active && probe.device_mode && address)
      return device_munmap(address, size);
   if (probe.active && probe.alias_mode && address &&
       (address == alias_probe.backing.mapping || address == alias_probe.view.mapping))
      return alias_munmap(address, size);
   if (probe.active && probe.memory_mode && address == probe.memory_mapping && address) {
      if (!probe.memory_active || size != 4096 || probe.memory_unmap_calls || probe.closed)
         return blocked();
      probe.memory_unmap_calls++;
      int result;
      if (!probe.live && cpu_case("unmap-fail")) {
         errno = EIO;
         result = -1;
      } else
         result = host_munmap(address, size);
      memory_result(4, result);
      if (!result) {
         probe.memory_mapping = NULL;
         probe.memory_allocated = false;
      } else
         probe.memory_cleanup_uncertain = true;
      printf("MEM_UNMAP ret=%d errno=%d no_mem_free_after_mapped_same_va=true\n", result, result ? errno : 0);
      return result;
   }
   if (probe.active && address == probe.tracking) {
      if (size != 4096 || probe.unmaps || probe.closed)
         return blocked();
      int result = host_munmap(address, size);
      if (!result)
         probe.unmaps++;
      return result;
   }
   return host_munmap(address, size);
}

int
close(int fd)
{
   if (probe.active && probe.device_mode && fd == probe.reopen_fd) {
      if (probe.reopen_closed)
         return blocked();
      int result = host_close(fd);
      if (!result) {
         probe.reopen_closed = true;
         probe.device_closes++;
      }
      return result;
   }
   if (probe.active && fd == probe.fd) {
      if (probe.closed || (probe.maps && probe.unmaps != probe.maps))
         return blocked();
      int result = host_close(fd);
      if (!result) {
         probe.closed = true;
         probe.closes++;
      }
      return result;
   }
   return host_close(fd);
}

ssize_t
read(int fd, void *data, size_t size)
{
   if (probe.active && probe.device_mode && (fd == probe.fd || fd == probe.reopen_fd))
      return device_read(fd, data, size);
   if (probe.active && fd == probe.fd)
      return blocked();
   return host_read(fd, data, size);
}

int
ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask)
{
   if (probe.active && probe.device_mode && fds && count == 1 &&
       (fds[0].fd == probe.fd || fds[0].fd == probe.reopen_fd))
      return device_ppoll(fds, count, timeout, mask);
   if (probe.active)
      return blocked();
   return host_ppoll(fds, count, timeout, mask);
}

struct filter_builder {
   struct sock_filter code[256];
   unsigned count;
};

static void
emit(struct filter_builder *b, struct sock_filter code)
{
   CHECK(b->count < sizeof(b->code) / sizeof(b->code[0]));
   b->code[b->count++] = code;
}

#define STMT(b, code, k) emit(b, (struct sock_filter)BPF_STMT(code, k))
#define JUMP(b, code, k, jt, jf) emit(b, (struct sock_filter)BPF_JUMP(code, k, jt, jf))
#define LOAD_ARG(b, n) STMT(b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[n]))
#define DENY(b) STMT(b, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
#define ALLOW(b) STMT(b, BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

static void
check_arg(struct filter_builder *b, unsigned argument, uint64_t value)
{
   STMT(b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]) + argument * 8);
   JUMP(b, BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)value, 1, 0);
   DENY(b);
   STMT(b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]) + argument * 8 + 4);
   JUMP(b, BPF_JMP | BPF_JEQ | BPF_K, value >> 32, 1, 0);
   DENY(b);
}

static void
emit_mmap_case(struct filter_builder *b, uint32_t size, int prot, uint64_t offset)
{
   unsigned skips[4], count = 0;
   LOAD_ARG(b, 1);
   JUMP(b, BPF_JMP | BPF_JEQ | BPF_K, size, 1, 0);
   skips[count++] = b->count;
   emit(b, (struct sock_filter)BPF_STMT(BPF_JMP | BPF_JA, 0));
   LOAD_ARG(b, 2);
   JUMP(b, BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)prot, 1, 0);
   skips[count++] = b->count;
   emit(b, (struct sock_filter)BPF_STMT(BPF_JMP | BPF_JA, 0));
   STMT(b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]) + 4);
   JUMP(b, BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)(offset >> 32), 1, 0);
   skips[count++] = b->count;
   emit(b, (struct sock_filter)BPF_STMT(BPF_JMP | BPF_JA, 0));
   LOAD_ARG(b, 5);
   JUMP(b, BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)offset, 1, 0);
   skips[count++] = b->count;
   emit(b, (struct sock_filter)BPF_STMT(BPF_JMP | BPF_JA, 0));
   ALLOW(b);
   for (unsigned i = 0; i < count; i++)
      b->code[skips[i]].k = b->count - skips[i] - 1;
}

static bool
install_guard(unsigned stage)
{
   struct filter_builder b = {0};
   STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
#if defined(__aarch64__)
   JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0);
#elif defined(__x86_64__)
   JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
#else
   return false;
#endif
   STMT(&b, BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
   STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
#if defined(__x86_64__)
   JUMP(&b, BPF_JMP | BPF_JSET | BPF_K, 0x40000000, 0, 1);
   STMT(&b, BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
#endif
#define BLOCK_NR(number) do { JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, number, 0, 1); DENY(&b); } while (0)
#ifdef SYS_open
   BLOCK_NR(SYS_open);
#endif
   BLOCK_NR(SYS_openat);
#ifdef SYS_openat2
   BLOCK_NR(SYS_openat2);
#endif
   unsigned ioctl_branch = b.count;
   JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 0, 0);
   if (probe.device_mode) {
      LOAD_ARG(&b, 0);
      STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]) + 4);
      JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
      DENY(&b);
      LOAD_ARG(&b, 0);
      JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)probe.fd, 2, 0);
      JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)probe.reopen_fd, 1, 0);
      DENY(&b);
   } else
      check_arg(&b, 0, probe.fd);
   STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1]) + 4);
   JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
   DENY(&b);
   {
      LOAD_ARG(&b, 1);
      const unsigned requests[] = {KBASE_IOCTL_VERSION_CHECK_CSF, KBASE_IOCTL_SET_FLAGS, KBASE_IOCTL_GET_GPUPROPS};
      for (unsigned i = 0; i < 3; i++) {
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, requests[i], 0, 1);
         ALLOW(&b);
      }
      if (probe.device_mode) {
         const unsigned device_requests[] = {
            KBASE_IOCTL_CS_GET_GLB_IFACE, KBASE_IOCTL_MEM_JIT_INIT, KBASE_IOCTL_MEM_ALLOC_EX,
            KBASE_IOCTL_MEM_FREE, KBASE_IOCTL_MEM_SYNC, KBASE_IOCTL_MEM_ALIAS,
            KBASE_IOCTL_CS_QUEUE_REGISTER, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18,
            KBASE_IOCTL_CS_QUEUE_BIND, KBASE_IOCTL_CS_QUEUE_KICK, KBASE_IOCTL_CS_QUEUE_TERMINATE,
            KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, KBASE_IOCTL_CS_TILER_HEAP_INIT,
            KBASE_IOCTL_CS_TILER_HEAP_TERM,
            /* Needed by the dma-buf cache-coherence probe: import a buffer the
             * GPU wrote into, then MEM_SYNC it before reading. */
            KBASE_IOCTL_MEM_IMPORT,
         };
         for (unsigned i = 0; i < sizeof(device_requests) / sizeof(device_requests[0]); i++) {
            JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, device_requests[i], 0, 1);
            ALLOW(&b);
         }
      }
      if (probe.csf_mode) {
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, KBASE_IOCTL_CS_GET_GLB_IFACE, 0, 1);
         ALLOW(&b);
      }
      if (probe.alias_mode) {
         const unsigned alias_requests[] = {KBASE_IOCTL_MEM_ALLOC_EX, KBASE_IOCTL_MEM_ALIAS, KBASE_IOCTL_MEM_FREE};
         for (unsigned i = 0; i < 3; i++) {
            JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, alias_requests[i], 0, 1);
            ALLOW(&b);
         }
      }
      if (probe.memory_mode) {
         const unsigned memory_requests[] = {KBASE_IOCTL_MEM_ALLOC_EX, KBASE_IOCTL_MEM_SYNC, KBASE_IOCTL_MEM_FREE};
         for (unsigned i = 0; i < 3; i++) {
            JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, memory_requests[i], 0, 1);
            ALLOW(&b);
         }
      }
   }
   DENY(&b);
   b.code[ioctl_branch].jf = b.count - ioctl_branch - 1;
   unsigned mmap_branch = b.count;
   JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, SYS_mmap, 0, 0);
   LOAD_ARG(&b, 4);
   JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, UINT32_MAX, 0, 4);
   LOAD_ARG(&b, 3);
   JUMP(&b, BPF_JMP | BPF_JSET | BPF_K, MAP_ANONYMOUS, 0, 1);
   ALLOW(&b);
   DENY(&b);
   {
      if (probe.device_mode) {
         /* mmap(fd) is args[4]; args[0] is the address and must be NULL. */
         LOAD_ARG(&b, 4);
         STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[4]) + 4);
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
         DENY(&b);
         LOAD_ARG(&b, 4);
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)probe.fd, 2, 0);
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)probe.reopen_fd, 1, 0);
         DENY(&b);
         check_arg(&b, 0, 0);
         check_arg(&b, 3, MAP_SHARED);
         emit_mmap_case(&b, 4096, PROT_NONE, BASE_MEM_MAP_TRACKING_HANDLE);
         emit_mmap_case(&b, 4096, PROT_READ, BASEP_MEM_CSF_USER_REG_PAGE_HANDLE);
         emit_mmap_case(&b, BASEP_QUEUE_NR_MMAP_USER_PAGES * 4096, PROT_READ | PROT_WRITE,
                        BASEP_MEM_CSF_USER_IO_PAGES_HANDLE);
         STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1]) + 4);
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
         DENY(&b);
         LOAD_ARG(&b, 1);
         JUMP(&b, BPF_JMP | BPF_JGE | BPF_K, 4096, 1, 0);
         DENY(&b);
         JUMP(&b, BPF_JMP | BPF_JGT | BPF_K, DEVICE_COOKIE_MAX_SIZE, 0, 1);
         DENY(&b);
         JUMP(&b, BPF_JMP | BPF_JSET | BPF_K, 4095, 0, 1);
         DENY(&b);
         LOAD_ARG(&b, 2);
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, PROT_READ, 2, 0);
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, PROT_READ | PROT_WRITE, 1, 0);
         DENY(&b);
         STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]) + 4);
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
         DENY(&b);
         LOAD_ARG(&b, 5);
         JUMP(&b, BPF_JMP | BPF_JGE | BPF_K, BASE_MEM_COOKIE_BASE, 1, 0);
         DENY(&b);
         JUMP(&b, BPF_JMP | BPF_JGE | BPF_K, BASE_MEM_FIRST_FREE_ADDRESS, 0, 1);
         DENY(&b);
         JUMP(&b, BPF_JMP | BPF_JSET | BPF_K, 4095, 0, 1);
         DENY(&b);
         ALLOW(&b);
      } else {
      check_arg(&b, 4, probe.fd);
      check_arg(&b, 0, 0);
      if (probe.alias_mode) {
         for (unsigned kind = 0; kind < 2; kind++) {
            uint64_t size = kind ? ALIAS_TOTAL_SIZE : ALIAS_BACKING_SIZE;
            LOAD_ARG(&b, 1);
            unsigned alias_branch = b.count;
            JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, size, 0, 0);
            check_arg(&b, 1, size);
            check_arg(&b, 2, kind ? PROT_READ : PROT_READ | PROT_WRITE);
            check_arg(&b, 3, MAP_SHARED);
            if ((!kind && stage >= 1) || (kind && stage >= 2))
               check_arg(&b, 5, kind ? alias_probe.view.cookie : alias_probe.backing.cookie);
            else {
               STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]) + 4);
               JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
               DENY(&b);
               LOAD_ARG(&b, 5);
               JUMP(&b, BPF_JMP | BPF_JGE | BPF_K, BASE_MEM_COOKIE_BASE, 1, 0);
               DENY(&b);
               JUMP(&b, BPF_JMP | BPF_JGE | BPF_K, BASE_MEM_FIRST_FREE_ADDRESS, 0, 1);
               DENY(&b);
               JUMP(&b, BPF_JMP | BPF_JSET | BPF_K, 4095, 0, 1);
               DENY(&b);
            }
            ALLOW(&b);
            b.code[alias_branch].jf = b.count - alias_branch - 1;
         }
      }
      check_arg(&b, 1, 4096);
      if (probe.memory_mode) {
         LOAD_ARG(&b, 2);
         unsigned memory_branch = b.count;
         JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, PROT_READ | PROT_WRITE, 0, 0);
         check_arg(&b, 2, PROT_READ | PROT_WRITE);
         check_arg(&b, 3, MAP_SHARED);
         if (stage)
            check_arg(&b, 5, probe.memory_cookie);
         else {
            STMT(&b, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[5]) + 4);
            JUMP(&b, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
            DENY(&b);
            LOAD_ARG(&b, 5);
            JUMP(&b, BPF_JMP | BPF_JGE | BPF_K, BASE_MEM_COOKIE_BASE, 1, 0);
            DENY(&b);
            JUMP(&b, BPF_JMP | BPF_JGE | BPF_K, BASE_MEM_FIRST_FREE_ADDRESS, 0, 1);
            DENY(&b);
            JUMP(&b, BPF_JMP | BPF_JSET | BPF_K, 4095, 0, 1);
            DENY(&b);
         }
         ALLOW(&b);
         b.code[memory_branch].jf = b.count - memory_branch - 1;
      }
      check_arg(&b, 2, PROT_NONE);
      check_arg(&b, 3, MAP_SHARED);
      check_arg(&b, 5, BASE_MEM_MAP_TRACKING_HANDLE);
      ALLOW(&b);
      }
   }
   DENY(&b);
   b.code[mmap_branch].jf = b.count - mmap_branch - 1;
   ALLOW(&b);
#undef BLOCK_NR
   struct sock_fprog program = {.len = b.count, .filter = b.code};
   return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
          prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

static void
guard_self_test(void)
{
   errno = 0;
   CHECK(syscall(SYS_openat, AT_FDCWD, "/home/zl/github/mesa/VERSION", O_RDONLY, 0) == -1 && errno == EPERM);
   errno = 0;
   CHECK(syscall(SYS_ioctl, -1, KBASE_IOCTL_GET_GPUPROPS, 0) == -1 && errno == EPERM);
   errno = 0;
   CHECK(syscall(SYS_ioctl, -1, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, 0) == -1 && errno == EPERM);
   if (!probe.live) {
      errno = 0;
      CHECK(syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_GET_GPUPROPS, 0) == -1 && errno == ENOTTY);
      const unsigned memory_requests[] = {KBASE_IOCTL_MEM_ALLOC_EX, KBASE_IOCTL_MEM_SYNC, KBASE_IOCTL_MEM_FREE};
      for (unsigned i = 0; i < 3; i++) {
         errno = 0;
         CHECK(syscall(SYS_ioctl, probe.fd, memory_requests[i], 0) == -1 &&
               errno == (probe.memory_mode || (probe.alias_mode && i != 1) ? ENOTTY : EPERM));
         errno = 0;
         CHECK(syscall(SYS_ioctl, -1, memory_requests[i], 0) == -1 && errno == EPERM);
         errno = 0;
         CHECK(syscall(SYS_ioctl, probe.fd, memory_requests[i] ^ (1u << _IOC_SIZESHIFT), 0) == -1 && errno == EPERM);
      }
      errno = 0;
      CHECK(syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, 0) == -1 &&
            errno == (probe.csf_mode ? ENOTTY : EPERM));
      errno = 0;
      CHECK(syscall(SYS_ioctl, -1, KBASE_IOCTL_CS_GET_GLB_IFACE, 0) == -1 && errno == EPERM);
      errno = 0;
      CHECK(syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE ^ (1u << _IOC_SIZESHIFT), 0) == -1 && errno == EPERM);
      errno = 0;
      CHECK(syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE ^ (1u << _IOC_DIRSHIFT), 0) == -1 && errno == EPERM);
      errno = 0;
      CHECK(syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, 0) == -1 && errno == EPERM);
      errno = 0;
      CHECK(syscall(SYS_ioctl, probe.fd, KBASE_IOCTL_GET_GPUPROPS ^ (1u << _IOC_SIZESHIFT), 0) == -1 && errno == EPERM);
      errno = 0;
      CHECK(syscall(SYS_mmap, 0, 4096, PROT_READ, MAP_SHARED, probe.fd, BASE_MEM_MAP_TRACKING_HANDLE) == -1 && errno == EPERM);
      errno = 0;
      CHECK(syscall(SYS_mmap, 0, 8192, PROT_NONE, MAP_SHARED, probe.fd, BASE_MEM_MAP_TRACKING_HANDLE) == -1 && errno == EPERM);
      errno = 0;
      CHECK(syscall(SYS_mmap, 0, 4096, PROT_NONE, MAP_SHARED, probe.fd, BASE_MEM_MAP_TRACKING_HANDLE + 4096) == -1 && errno == EPERM);
      void *tracking = (void *)syscall(SYS_mmap, 0, 4096, PROT_NONE, MAP_SHARED, probe.fd, BASE_MEM_MAP_TRACKING_HANDLE);
      CHECK(tracking != MAP_FAILED);
      CHECK(host_munmap(tracking, 4096) == 0);
      unsigned expected = probe.blocked;
      probe.active = true;
      struct kbase_ioctl_set_flags flags = {0};
      CHECK(ioctl(probe.fd, KBASE_IOCTL_SET_FLAGS, &flags) == -1);
      CHECK(ioctl(probe.fd, KBASE_IOCTL_GET_GPUPROPS ^ (1u << _IOC_SIZESHIFT), &flags) == -1);
      CHECK(open("/dev/mali9", O_RDWR | O_CLOEXEC) == -1);
      union kbase_ioctl_cs_get_glb_iface query = {0};
      CHECK(ioctl(probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &query) == -1);
      union kbase_ioctl_mem_alloc_ex allocation = {0};
      CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_ALLOC_EX, &allocation) == -1);
      probe.active = false;
      CHECK(probe.blocked == expected + 5);
      probe.blocked = expected;
   }
}

static void
dump_properties(void)
{
   if (!probe.size)
      return;
   blake3_hash hash;
   char hex[BLAKE3_HEX_LEN];
   _mesa_blake3_compute(probe.properties, probe.size, hash);
   _mesa_blake3_format(hex, hash);
   printf("GPUPROPS bytes=%u blake3=%s\n", probe.size, hex);
   size_t offset = 0;
   while (offset < probe.size) {
      if (probe.size - offset < 4) {
         printf("malformed property header at %zu\n", offset);
         return;
      }
      uint32_t header;
      memcpy(&header, probe.properties + offset, 4);
      unsigned size = 1u << (header & 3);
      if (size > probe.size - offset - 4) {
         printf("malformed property value at %zu\n", offset);
         return;
      }
      uint64_t value = 0;
      memcpy(&value, probe.properties + offset + 4, size);
      printf("GPUPROP key=%u size=%u value=0x%llx\n", header >> 2, size, (unsigned long long)value);
      offset += 4 + size;
   }
}

static void
print_csf_snapshot(void)
{
   for (unsigned i = 0; i < probe.csf_calls; i++) {
      const union kbase_ioctl_cs_get_glb_iface *r = &probe.csf_phases[i];
      printf("CSF_PHASE index=%u ret=%d errno=%d version=0x%x features=0x%x groups=%u prfcnt_size=%u streams=%u instr_features=0x%x\n",
             i + 1, probe.csf_results[i], probe.csf_errors[i], r->out.glb_version,
             r->out.features, r->out.group_num, r->out.prfcnt_size,
             r->out.total_stream_num, r->out.instr_features);
   }
   if (probe.csf_calls != 2 || probe.csf_results[1] || !probe.groups || !probe.streams)
      return;
   for (unsigned i = 0; i < probe.csf_groups; i++)
      printf("CSF_GROUP index=%u features=0x%x streams=%u suspend_size=%u padding=0x%x\n",
             i, probe.groups[i].features, probe.groups[i].stream_num,
             probe.groups[i].suspend_size, probe.groups[i].padding);
   for (unsigned i = 0; i < probe.csf_streams; i++)
      printf("CSF_STREAM index=%u features=0x%x work_register_field=%u scoreboard_field=%u padding=0x%x\n",
             i, probe.streams[i].features, probe.streams[i].features & 0xff,
             (probe.streams[i].features >> 8) & 0xff, probe.streams[i].padding);
   struct mesa_blake3 hasher;
   blake3_hash hash;
   char hex[BLAKE3_HEX_LEN];
   _mesa_blake3_init(&hasher);
   _mesa_blake3_update(&hasher, &probe.csf_phases[0].out, sizeof(probe.csf_phases[0].out));
   _mesa_blake3_update(&hasher, &probe.csf_phases[1].out, sizeof(probe.csf_phases[1].out));
   _mesa_blake3_update(&hasher, probe.groups, probe.csf_groups * sizeof(*probe.groups));
   _mesa_blake3_update(&hasher, probe.streams, probe.csf_streams * sizeof(*probe.streams));
   _mesa_blake3_final(&hasher, hash);
   _mesa_blake3_format(hex, hash);
   printf("CSF_RAW_DIGEST order=phase1_out,phase2_out,group_capacity_rows,stream_capacity_rows encoding=little-endian bytes=%zu blake3=%s\n",
          sizeof(probe.csf_phases[0].out) * 2 + probe.csf_groups * sizeof(*probe.groups) +
          probe.csf_streams * sizeof(*probe.streams), hex);
}

static bool
query_csf_capabilities(void)
{
   CHECK(probe.csf_mode && !probe.csf_calls && !probe.csf_query_active);
   CHECK(probe.handed_off && !probe.closed && probe.version_calls == 1 &&
         probe.flags_calls == 1 && probe.props_calls == 2 && probe.maps == 1 &&
         !probe.unmaps && !probe.closes && !probe.blocked);
   probe.csf_query_active = true;
   probe.csf_reason = "phase1-ioctl";
   union kbase_ioctl_cs_get_glb_iface request = {0};
   if (!probe.live) {
      unsigned denied = probe.blocked;
      union kbase_ioctl_cs_get_glb_iface wrong = {.in.max_group_num = 1};
      CHECK(ioctl(probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &wrong) == -1 && !probe.csf_calls);
      CHECK(probe.blocked == denied + 1);
      probe.blocked = denied;
   }
   bool accepted = false;
   if (ioctl(probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &request))
      goto out;
   probe.csf_groups = request.out.group_num;
   probe.csf_streams = request.out.total_stream_num;
   probe.csf_reason = "count-bounds";
   if (!probe.csf_groups || probe.csf_groups > CSF_MAX_GROUPS ||
       !probe.csf_streams || probe.csf_streams > CSF_MAX_STREAMS ||
       probe.csf_groups > SIZE_MAX / sizeof(*probe.groups) ||
       probe.csf_streams > SIZE_MAX / sizeof(*probe.streams))
      goto out;
   probe.groups = calloc(probe.csf_groups, sizeof(*probe.groups));
   probe.streams = calloc(probe.csf_streams, sizeof(*probe.streams));
   probe.csf_reason = "host-allocation";
   if (!probe.groups || !probe.streams)
      goto out;
   request = (union kbase_ioctl_cs_get_glb_iface){0};
   request.in.max_group_num = probe.csf_groups;
   request.in.max_total_stream_num = probe.csf_streams;
   request.in.groups_ptr = (uintptr_t)probe.groups;
   request.in.streams_ptr = (uintptr_t)probe.streams;
   if (!probe.live) {
      unsigned denied = probe.blocked;
      union kbase_ioctl_cs_get_glb_iface wrong = request;
      wrong.in.max_total_stream_num++;
      CHECK(ioctl(probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &wrong) == -1 && probe.csf_calls == 1);
      wrong = request;
      wrong.in.groups_ptr += sizeof(*probe.groups);
      CHECK(ioctl(probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &wrong) == -1 && probe.csf_calls == 1);
      CHECK(probe.blocked == denied + 2);
      probe.blocked = denied;
   }
   probe.csf_reason = "phase2-ioctl";
   if (ioctl(probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &request))
      goto out;
   probe.csf_reason = "snapshot-change";
   if (memcmp(&probe.csf_phases[0].out, &probe.csf_phases[1].out,
              sizeof(probe.csf_phases[0].out)))
      goto out;
   probe.csf_reason = "stream-sum";
   uint64_t stream_sum = 0;
   unsigned streams_per_group = 0;
   for (unsigned i = 0; i < probe.csf_groups; i++) {
      uint32_t n = probe.groups[i].stream_num;
      if (!n || n > probe.csf_streams || n > probe.csf_streams - stream_sum)
         goto out;
      stream_sum += n;
      if (n > streams_per_group)
         streams_per_group = n;
   }
   if (stream_sum != probe.csf_streams)
      goto out;
   probe.csf_reason = "stream-feature-uniformity";
   for (unsigned i = 1; i < probe.csf_streams; i++)
      if (probe.streams[i].features != probe.streams[0].features)
         goto out;
   probe.csf_reason = "scoreboard-policy";
   unsigned scoreboard_count = (probe.streams[0].features >> 8) & 0xff;
   if (scoreboard_count < 4 || scoreboard_count > 16)
      goto out;
   printf("CSF_COMPATIBILITY group_slots=%u max_streams_per_group=%u scoreboard_slots=%u cs_reg_count_policy=128 unpreserved_policy=4 registers_are_not_query_evidence=true\n",
          probe.csf_groups, streams_per_group, scoreboard_count);
   probe.csf_reason = "accepted";
   accepted = true;
out:
   probe.csf_query_active = false;
   if (!probe.live) {
      unsigned denied = probe.blocked;
      union kbase_ioctl_cs_get_glb_iface wrong = {0};
      CHECK(ioctl(probe.fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &wrong) == -1);
      CHECK(probe.blocked == denied + 1);
      probe.blocked = denied;
   }
   print_csf_snapshot();
   printf("CSF_VERDICT accepted=%u reason=%s attempts=%u\n", accepted, probe.csf_reason, probe.csf_calls);
   free(probe.groups);
   free(probe.streams);
   probe.groups = NULL;
   probe.streams = NULL;
   return accepted;
}

static uint8_t
memory_pattern(unsigned index)
{
   return probe.event_mode ? 0 : (uint8_t)((index * 37u) ^ (index >> 3) ^ 0xa5u);
}

static bool
exercise_memory(void)
{
   CHECK(probe.memory_mode && !probe.csf_mode && probe.handed_off && !probe.closed);
   CHECK(probe.props_calls == 2 && probe.maps == 1 && !probe.unmaps && !probe.closes);
   sigset_t mask;
   sigfillset(&mask);
   CHECK(sigprocmask(SIG_SETMASK, &mask, NULL) == 0);
   printf("MEM_PAGE_KIND kind=%s flags_in=0x%llx\n", probe.event_mode ? "CSF_EVENT" : "ordinary",
          (unsigned long long)memory_input_flags());
   probe.memory_active = true;
   probe.memory_reason = "allocation-failed";
   bool accepted = false;
   union kbase_ioctl_mem_alloc_ex allocation = {
      .in = {.va_pages = 1, .commit_pages = 1, .flags = memory_input_flags()},
   };
   if (!probe.live) {
      unsigned denied = probe.blocked;
      union kbase_ioctl_mem_alloc_ex wrong = allocation;
      wrong.in.commit_pages = 2;
      CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_ALLOC_EX, &wrong) == -1);
      CHECK(!probe.memory_alloc_calls && probe.blocked == denied + 1);
      probe.blocked = denied;
   }
   if (ioctl(probe.fd, KBASE_IOCTL_MEM_ALLOC_EX, &allocation))
      goto out;
   if (!probe.live) {
      unsigned denied = probe.blocked;
      union kbase_ioctl_mem_alloc_ex duplicate = {
         .in = {.va_pages = 1, .commit_pages = 1, .flags = memory_input_flags()},
      };
      CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_ALLOC_EX, &duplicate) == -1);
      CHECK(probe.memory_alloc_calls == 1 && probe.blocked == denied + 1);
      probe.blocked = denied;
   }
   uint64_t required = BASE_MEM_SAME_VA | BASE_MEM_PROT_CPU_RD |
                       BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |
                       BASE_MEM_PROT_GPU_WR;
   if (probe.event_mode)
      required |= BASE_MEM_CSF_EVENT;
   else
      required |= BASE_MEM_CACHED_CPU;
   uint64_t forbidden = BASE_MEM_PROT_GPU_EX |
                        BASE_MEM_UNCACHED_GPU | BASE_MEM_PROTECTED |
                        BASE_MEM_IMPORT_SHARED | BASE_MEM_GROW_ON_GPF;
   if (!probe.event_mode)
      forbidden |= BASE_MEM_CSF_EVENT;
   probe.memory_reason = "invalid-output";
   if (!valid_memory_cookie(probe.memory_cookie) ||
       (probe.memory_output_flags & required) != required ||
       (probe.memory_output_flags & forbidden)) {
      probe.memory_cleanup_uncertain = true;
      goto out;
   }
   probe.memory_output_valid = true;
   probe.memory_reason = "cookie-guard-failed";
   if (!install_guard(true)) {
      if (probe.event_mode) {
         probe.memory_cleanup_uncertain = true;
         goto out;
      }
      goto free_unmapped;
   }
   probe.memory_cookie_guard = true;
   if (!probe.live) {
      errno = 0;
      CHECK(syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                    probe.fd, probe.memory_cookie + 4096) == -1 && errno == EPERM);
      errno = 0;
      CHECK(syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED,
                    probe.fd, probe.memory_cookie) == -1 && errno == EPERM);
      void *test = (void *)syscall(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, probe.fd, probe.memory_cookie);
      CHECK(test != MAP_FAILED);
      CHECK(host_munmap(test, 4096) == 0);
      unsigned denied = probe.blocked;
      CHECK(mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED,
                 probe.fd, probe.memory_cookie) == MAP_FAILED);
      CHECK(!probe.memory_map_calls && probe.blocked == denied + 1);
      probe.blocked = denied;
   }
   probe.memory_reason = "mapping-failed";
   void *ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                    probe.fd, probe.memory_cookie);
   if (ptr == MAP_FAILED)
      goto free_unmapped;
   if ((uintptr_t)ptr & 4095) {
      probe.memory_reason = "mapping-alignment";
      goto unmap;
   }
   volatile uint8_t *bytes = ptr;
   for (unsigned i = 0; i < 4096; i++)
      bytes[i] = memory_pattern(i);
   struct kbase_ioctl_mem_sync sync = {
      .handle = (uintptr_t)ptr, .user_addr = (uintptr_t)ptr, .size = 4096,
      .type = BASE_SYNCSET_OP_MSYNC,
   };
   probe.memory_reason = "clean-failed";
   if (!probe.live) {
      unsigned denied = probe.blocked;
      struct kbase_ioctl_mem_sync wrong = sync;
      wrong.handle = probe.memory_cookie;
      CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_SYNC, &wrong) == -1);
      CHECK(!probe.memory_sync_calls && probe.blocked == denied + 1);
      probe.blocked = denied;
   }
   if (ioctl(probe.fd, KBASE_IOCTL_MEM_SYNC, &sync))
      goto unmap;
   sync.type = BASE_SYNCSET_OP_CSYNC;
   probe.memory_reason = "invalidate-failed";
   if (ioctl(probe.fd, KBASE_IOCTL_MEM_SYNC, &sync))
      goto unmap;
   probe.memory_reason = "cpu-readback-mismatch";
   for (unsigned i = 0; i < 4096; i++)
      if (bytes[i] != memory_pattern(i))
         goto unmap;
   if (probe.event_mode) {
      uint64_t seqno;
      uint32_t error;
      memcpy(&seqno, ptr, sizeof(seqno));
      memcpy(&error, (uint8_t *)ptr + 8, sizeof(error));
      CHECK(seqno == 0 && error == 0);
      printf("CSF_EVENT_CPU_STATE seqno=%llu error=%u page_zero=true queue_bound=false gpu_completion_unproven=true\n",
             (unsigned long long)seqno, error);
   }
   blake3_hash hash;
   char hex[BLAKE3_HEX_LEN];
   _mesa_blake3_compute(ptr, 4096, hash);
   _mesa_blake3_format(hex, hash);
   printf("MEM_CPU_READBACK bytes=4096 blake3=%s gpu_execution=false gpu_visibility_unproven=true\n", hex);
   probe.memory_reason = "accepted";
   accepted = true;
unmap:
   if (munmap(probe.memory_mapping, 4096)) {
      accepted = false;
      probe.memory_reason = "unmap-failed";
   }
   goto out;
free_unmapped: {
   struct kbase_ioctl_mem_free free_request = {.gpu_addr = probe.memory_cookie};
   if (ioctl(probe.fd, KBASE_IOCTL_MEM_FREE, &free_request))
      probe.memory_reason = "unmapped-free-failed";
}
out:
   probe.memory_active = false;
   if (!probe.live) {
      unsigned denied = probe.blocked;
      struct kbase_ioctl_mem_free wrong = {.gpu_addr = probe.memory_cookie};
      CHECK(ioctl(probe.fd, KBASE_IOCTL_MEM_FREE, &wrong) == -1);
      CHECK(probe.blocked == denied + 1);
      probe.blocked = denied;
   }
   printf("MEM_VERDICT accepted=%u reason=%s allocated=%u mapping_live=%u cleanup_uncertain=%u cookie_guard=%u\n",
          accepted, probe.memory_reason, probe.memory_allocated,
          probe.memory_mapping != NULL, probe.memory_cleanup_uncertain, probe.memory_cookie_guard);
   for (unsigned operation = 0; operation < 6; operation++)
      if (probe.memory_operation_mask & (1u << operation))
         printf("MEM_OPERATION index=%u ret=%d errno=%d\n", operation,
                probe.memory_results[operation], probe.memory_errors[operation]);
   return accepted;
}

static int
run_probe(const char *library, bool live, bool csf_mode, bool memory_mode, bool event_mode, bool alias_mode, bool device_mode, bool submit_mode, const char *fixture)
{
   CHECK(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
   probe.fd = -1;
   probe.reopen_fd = -1;
   probe.live = live;
   probe.csf_mode = csf_mode;
   probe.memory_mode = memory_mode;
   probe.event_mode = event_mode;
   probe.alias_mode = alias_mode;
   probe.device_mode = device_mode;
   probe.submit_mode = submit_mode;
   CHECK(!device_mode || (!csf_mode && !memory_mode && !event_mode && !alias_mode));
   CHECK(!alias_mode || (!csf_mode && !memory_mode && !event_mode));
   CHECK(!event_mode || (memory_mode && !csf_mode));
   CHECK(!csf_mode || !memory_mode);
   probe.cpu_case = fixture;
   resolve_host();
   setenv("PANVK_KBASE_DEVICE", "/dev/mali0", 1);
   setenv("PAN_KMOD_RESTRICT_TO_BACKENDS", "kbase", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
   /* The probe pins PANVK_DEBUG off so baselines stay reproducible.  A caller
    * that explicitly wants driver-side debug categories (for example the
    * alias_writeback probe) passes them through PANVK_KBASE_PROBE_DEBUG, which
    * keeps the default isolated while making the choice visible at the call
    * site.
    */
   const char *probe_debug = getenv("PANVK_KBASE_PROBE_DEBUG");
   if (probe_debug && *probe_debug)
      setenv("PANVK_DEBUG", probe_debug, 1);
   else
      unsetenv("PANVK_DEBUG");
   void *icd = dlopen(library, RTLD_NOW | RTLD_LOCAL);
   if (!icd) {
      fprintf(stderr, "dlopen failed: %s\n", dlerror());
      return 1;
   }
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
   CHECK(gipa);
   Dl_info image;
   char expected_path[4096], loaded_path[4096];
   CHECK(dladdr((void *)gipa, &image));
   CHECK(realpath(library, expected_path) && realpath(image.dli_fname, loaded_path));
   CHECK(!strcmp(expected_path, loaded_path));
   const struct panvk_icd_binding bindings[] = {
      {"open64", (void *)open64}, {"ioctl", (void *)ioctl},
      {"mmap64", (void *)mmap64}, {"munmap", (void *)munmap},
      {"close", (void *)close}, {"read", (void *)read}, {"ppoll", (void *)ppoll},
   };
   CHECK(panvk_icd_verify_bindings((void *)gipa, bindings, sizeof(bindings) / sizeof(bindings[0])));
   if (live) {
      probe.fd = host_open("/dev/mali0", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
      if (probe.fd < 0) {
         fprintf(stderr, "Live probe blocked before handshake: open /dev/mali0: %s\n", strerror(errno));
         return 1;
      }
      struct stat stat;
      if (fstat(probe.fd, &stat) || !S_ISCHR(stat.st_mode)) {
         host_close(probe.fd);
         fprintf(stderr, "Selected node is not a verified character device\n");
         return 1;
      }
   } else {
      probe.fd = memfd_create("panvk-enumeration-probe", MFD_CLOEXEC);
      CHECK(probe.fd >= 0 && ftruncate(probe.fd, alias_mode ? BASE_MEM_FIRST_FREE_ADDRESS + ALIAS_TOTAL_SIZE : 0x4000) == 0);
      if (alias_mode)
         alias_prepare_cpu();
   }
   if (device_mode) {
      char reopen_path[64];
      snprintf(reopen_path, sizeof(reopen_path), "/proc/self/fd/%d", probe.fd);
      probe.reopen_fd = host_open(reopen_path, O_RDWR | O_CLOEXEC);
      if (probe.reopen_fd < 0) {
         fprintf(stderr, "Logical-device descriptor pre-open failed: %s\n", strerror(errno));
         host_close(probe.fd);
         return 1;
      }
      if (live) {
         struct stat stat;
         if (fstat(probe.reopen_fd, &stat) || !S_ISCHR(stat.st_mode)) {
            host_close(probe.reopen_fd);
            host_close(probe.fd);
            fprintf(stderr, "Reopened descriptor is not the verified character device\n");
            return 1;
         }
      }
      device_probe.unused_fd = memfd_create("panvk-device-unrelated", MFD_CLOEXEC);
      CHECK(device_probe.unused_fd >= 0);
   }
   if (!install_guard(0)) {
      host_close(probe.fd);
      fprintf(stderr, "Enumeration syscall guard unavailable: %s\n", strerror(errno));
      return 1;
   }
   if (device_mode) {
      if (!live) {
         /* The kernel rejects most device ioctls with EPERM until its context is
          * set up, so the allow/deny envelope is only verifiable against a plain
          * descriptor; the live run is bounded by the installed filter plus the
          * wrapper ledger (blocked must stay zero) instead. */
         if (!device_guard_self_test() || !device_sequence_self_test()) {
            fprintf(stderr, "Device syscall guard self-test failed\n");
            return 1;
         }
         /* --cpu-submit additionally walks the real ICD submit path; it is the
          * CPU counterpart of --live-submit, so it must not stop at the
          * self-test. --cpu-device keeps the historical early return. */
         if (!submit_mode) {
            printf("DEVICE_CPU_SELFTEST_OK icd_path_covered_by=panvk_kbase_icd_suite\n");
            return 0;
         }
      }
   } else {
      guard_self_test();
      if (alias_mode && !live)
         alias_guard_self_test(0);
   }
   probe.active = true;
   PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
   CHECK(create_instance && panvk_icd_symbol_in_image((void *)gipa, (void *)create_instance));
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(&info, NULL, &instance);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "CreateInstance returned %d\n", result);
      if (!probe.closed)
         close(probe.fd);
      return 1;
   }
#define INSTANCE_PROC(type, name) type name = (type)gipa(instance, "vk" #name); CHECK(name && panvk_icd_symbol_in_image((void *)gipa, (void *)name))
   INSTANCE_PROC(PFN_vkEnumeratePhysicalDevices, EnumeratePhysicalDevices);
   INSTANCE_PROC(PFN_vkGetPhysicalDeviceProperties, GetPhysicalDeviceProperties);
   INSTANCE_PROC(PFN_vkGetPhysicalDeviceFeatures, GetPhysicalDeviceFeatures);
   INSTANCE_PROC(PFN_vkGetPhysicalDeviceMemoryProperties, GetPhysicalDeviceMemoryProperties);
   INSTANCE_PROC(PFN_vkGetPhysicalDeviceQueueFamilyProperties, GetPhysicalDeviceQueueFamilyProperties);
   INSTANCE_PROC(PFN_vkDestroyInstance, DestroyInstance);
#undef INSTANCE_PROC
   uint32_t count = 0;
   result = EnumeratePhysicalDevices(instance, &count, NULL);
   printf("ENUMERATION mode=%s result=%d count=%u\n", live ? "LIVE" : "CPU", result, count);
   bool success = result == VK_SUCCESS && count == 1;
   VkPhysicalDevice device_physical = VK_NULL_HANDLE;
   if (success) {
      VkPhysicalDevice physical;
      count = 1;
      result = EnumeratePhysicalDevices(instance, &count, &physical);
      success = result == VK_SUCCESS && count == 1;
      if (success) {
         device_physical = physical;
         VkPhysicalDeviceProperties properties;
         VkPhysicalDeviceFeatures features;
         VkPhysicalDeviceMemoryProperties memory;
         GetPhysicalDeviceProperties(physical, &properties);
         GetPhysicalDeviceFeatures(physical, &features);
         GetPhysicalDeviceMemoryProperties(physical, &memory);
         printf("DEVICE name=%s api=%u.%u.%u vendor=0x%x device=0x%x\n", properties.deviceName,
                VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
                VK_API_VERSION_PATCH(properties.apiVersion), properties.vendorID, properties.deviceID);
         unsigned optional = 0;
         for (unsigned i = 0; i < sizeof(features) / sizeof(VkBool32); i++)
            optional += ((VkBool32 *)&features)[i] != 0;
         printf("CAPABILITIES optional_core_features=%u memory_heaps=%u memory_types=%u\n", optional, memory.memoryHeapCount, memory.memoryTypeCount);
         /* Each of these is declared only because a hardware probe showed the
          * behaviour; nothing else is declared. */
         CHECK(features.samplerAnisotropy && features.depthClamp &&
               features.depthBiasClamp && features.independentBlend);
         optional -= 4;
         uint32_t family_count = 0;
         GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
         CHECK(family_count <= 4);
         VkQueueFamilyProperties families[4];
         GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
         for (unsigned i = 0; i < family_count; i++)
            printf("QUEUE_FAMILY index=%u queues=%u flags=0x%x timestamp_bits=%u\n", i, families[i].queueCount, families[i].queueFlags, families[i].timestampValidBits);
         success &= properties.apiVersion == VK_API_VERSION_1_0 && optional == 0 && family_count == 1 &&
                    families[0].queueCount == 1 && families[0].timestampValidBits == 0;
      }
   }
   bool csf_accepted = false;
   bool memory_accepted = false;
   bool alias_accepted = false;
   bool device_accepted = false;
   if (success && csf_mode)
      csf_accepted = query_csf_capabilities();
   if (success && memory_mode)
      memory_accepted = exercise_memory();
   if (success && alias_mode)
      alias_accepted = exercise_alias();
   if (success && device_mode) {
      VkPhysicalDevice physical = device_physical;
      PFN_vkGetDeviceProcAddr gdpa_check = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
      CHECK(gdpa_check);
      device_accepted = exercise_device(instance, gipa);
      (void)physical;
   }
   DestroyInstance(instance, NULL);
   if (!live && success) {
      unsigned denied = probe.blocked;
      CHECK(open("/dev/mali0", O_RDWR | O_CLOEXEC) == -1 && errno == EPERM);
      CHECK(probe.blocked == denied + 1);
      probe.blocked = denied;
   }
   dump_properties();
   printf("SAFETY version=%u set_flags=%u props=%u maps=%u unmaps=%u closes=%u blocked=%u\n",
          probe.version_calls, probe.flags_calls, probe.props_calls, probe.maps, probe.unmaps, probe.closes, probe.blocked);
   if (!probe.closed)
      close(probe.fd);
   if (device_mode) {
      success &= probe.handed_off && probe.closed && probe.device_opens == 1 && probe.reopen_closed;
      success &= device_accepted && device_check_result();
   } else {
      success &= probe.handed_off && probe.closed && probe.version_calls == 1 && probe.flags_calls == 1 &&
                 probe.props_calls == 2 && probe.maps == 1 && probe.unmaps == 1 && probe.closes == 1 && !probe.blocked;
   }
   if (csf_mode) {
      bool expected_rejection = !live && fixture && strcmp(fixture, "positive");
      const char *expected_reason = cpu_case("count-change") ? "snapshot-change" :
         cpu_case("bad-sum") ? "stream-sum" :
         cpu_case("nonuniform") ? "stream-feature-uniformity" :
         cpu_case("scoreboard") ? "scoreboard-policy" : "accepted";
      success &= probe.csf_calls == 2 && !probe.csf_query_active &&
                 csf_accepted != expected_rejection && probe.csf_reason &&
                 !strcmp(probe.csf_reason, expected_reason);
      printf("CSF_SAFETY calls=%u query_active=%u expected_rejection=%u\n",
             probe.csf_calls, probe.csf_query_active, expected_rejection);
   }
   if (memory_mode) {
      const char *expected_reason = cpu_case("allocation-fail") ? "allocation-failed" :
         cpu_case("mapping-fail") ? "mapping-failed" :
         cpu_case("clean-fail") ? "clean-failed" :
         cpu_case("invalidate-fail") ? "invalidate-failed" :
         cpu_case("invalid-output") || cpu_case("invalid-flags") || cpu_case("missing-event") ? "invalid-output" :
         cpu_case("free-fail") ? "unmapped-free-failed" :
         cpu_case("unmap-fail") ? "unmap-failed" : "accepted";
      bool expected_rejection = !live && strcmp(fixture, "positive");
      success &= memory_accepted != expected_rejection && probe.memory_reason &&
                 !strcmp(probe.memory_reason, expected_reason) &&
                 probe.memory_alloc_calls == 1 && !probe.memory_active;
      if (cpu_case("invalid-output") || cpu_case("invalid-flags") || cpu_case("missing-event"))
         success &= probe.memory_cleanup_uncertain && !probe.memory_map_calls && !probe.memory_free_calls;
      else if (cpu_case("free-fail"))
         success &= probe.memory_cleanup_uncertain && probe.memory_free_calls == 1 &&
                    probe.memory_allocated && !probe.memory_mapping;
      else if (cpu_case("unmap-fail"))
         success &= probe.memory_cleanup_uncertain && probe.memory_unmap_calls == 1 &&
                    probe.memory_allocated && probe.memory_mapping;
      else
         success &= !probe.memory_allocated && !probe.memory_mapping && !probe.memory_cleanup_uncertain;
      if (memory_accepted)
         success &= probe.memory_map_calls == 1 && probe.memory_unmap_calls == 1 &&
                    probe.memory_sync_calls == 2 && !probe.memory_free_calls && probe.memory_cookie_guard;
      if (cpu_case("mapping-fail"))
         success &= probe.memory_free_calls == 1 && !probe.memory_unmap_calls && !probe.memory_sync_calls;
      printf("MEM_SAFETY alloc=%u maps=%u syncs=%u unmaps=%u frees=%u active=%u expected_rejection=%u cleanup_uncertain=%u\n",
             probe.memory_alloc_calls, probe.memory_map_calls, probe.memory_sync_calls,
             probe.memory_unmap_calls, probe.memory_free_calls, probe.memory_active,
             expected_rejection, probe.memory_cleanup_uncertain);
   }
   if (alias_mode) {
      success &= alias_check_result(alias_accepted);
      if (!live)
         alias_cleanup_cpu();
   }
   fflush(stdout);
   return success ? 0 : 1;
}

int
main(int argc, char **argv)
{
   bool submit = argc >= 2 && (!strcmp(argv[1], "--cpu-submit") || !strcmp(argv[1], "--live-submit"));
   bool device = submit || (argc >= 2 && (!strcmp(argv[1], "--cpu-device") || !strcmp(argv[1], "--live-device")));
   bool alias = argc >= 2 && (!strcmp(argv[1], "--cpu-alias") || !strcmp(argv[1], "--live-alias"));
   bool event = argc >= 2 && (!strcmp(argv[1], "--cpu-event") || !strcmp(argv[1], "--live-event"));
   bool memory = event || (argc >= 2 && (!strcmp(argv[1], "--cpu-memory") || !strcmp(argv[1], "--live-memory")));
   bool cpu = argc >= 2 && (!strcmp(argv[1], "--cpu") || !strcmp(argv[1], "--cpu-csf") || !strcmp(argv[1], "--cpu-memory") || !strcmp(argv[1], "--cpu-event") || !strcmp(argv[1], "--cpu-alias") || !strcmp(argv[1], "--cpu-device") || !strcmp(argv[1], "--cpu-submit"));
   bool live = argc >= 2 && (!strcmp(argv[1], "--live") || !strcmp(argv[1], "--live-csf") || !strcmp(argv[1], "--live-memory") || !strcmp(argv[1], "--live-event") || !strcmp(argv[1], "--live-alias") || !strcmp(argv[1], "--live-device") || !strcmp(argv[1], "--live-submit"));
   bool csf = argc >= 2 && (!strcmp(argv[1], "--cpu-csf") || !strcmp(argv[1], "--live-csf"));
   if ((!cpu && !live) || (argc != 3 && !(argc == 4 && cpu && (csf || memory || alias || device)))) {
      fprintf(stderr, "usage: %s --cpu|--live|--cpu-csf|--live-csf|--cpu-memory|--live-memory|--cpu-event|--live-event|--cpu-alias|--live-alias|--cpu-device|--live-device|--cpu-submit|--live-submit /absolute/path/libvulkan_panfrost.so [cpu-fixture]\n", argv[0]);
      return 1;
   }
   const char *fixture = argc == 4 ? argv[3] : "positive";
   const char *fixtures[] = {"positive", "count-change", "bad-sum", "nonuniform", "scoreboard",
                            "allocation-fail", "mapping-fail", "clean-fail", "invalidate-fail",
                            "invalid-output", "invalid-flags", "free-fail", "unmap-fail", "missing-event"};
   bool known = false;
   for (unsigned i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++)
      known |= !strcmp(fixture, fixtures[i]);
   if (alias) {
      known = false;
      for (unsigned i = 0; i < sizeof(alias_fixtures) / sizeof(alias_fixtures[0]); i++)
         known |= !strcmp(fixture, alias_fixtures[i].name);
   }
   if (device)
      known = !strcmp(fixture, "positive");
   CHECK(known);
   CHECK(argv[2][0] == '/');
   pid_t child = fork();
   CHECK(child >= 0);
   if (!child)
      _exit(run_probe(argv[2], live, csf, memory, event, alias, device, submit, fixture));
   int status = 0;
   /* Queue initialisation submits the init stream and waits for its completion
    * event, so the logical-device window needs a longer userspace deadline. */
   unsigned iterations = device ? 1200 : 300;
   for (unsigned i = 0; i < iterations; i++) {
      pid_t result = waitpid(child, &status, WNOHANG);
      CHECK(result >= 0);
      if (result == child) {
         if (WIFSIGNALED(status))
            fprintf(stderr, "Probe child terminated by signal %d\n", WTERMSIG(status));
         return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
      }
      struct timespec delay = {.tv_nsec = 50000000};
      nanosleep(&delay, NULL);
   }
   kill(child, SIGKILL);
   waitpid(child, &status, 0);
   fprintf(stderr, "Probe userspace deadline exceeded; kernel completion is not established\n");
   return 1;
}
