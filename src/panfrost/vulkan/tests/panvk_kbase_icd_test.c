#include "panvk_kbase_icd_guard.h"
#include "drm-uapi/mali_kbase_ioctl.h"
#include "genxml/gen_macros.h"
#include "genxml/decode.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define PAGE 4096u
#define MAX_CONTEXTS 8
#define MAX_BOS 512
#define MAX_MAPS 1024
#define MAX_QUEUES 8
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)

struct context {
   int fd;
   unsigned generation;
   unsigned jit;
   unsigned queue_count;
   unsigned heaps_created;
   unsigned heaps_live;
   bool live;
};

struct bo {
   struct context *ctx;
   uint64_t cookie;
   uint64_t flags;
   uint64_t gpu;
   size_t size;
   int backing;
   struct bo *source;
   unsigned repeats;
   bool live;
};

struct mapping {
   void *ptr;
   size_t size;
   struct context *ctx;
   struct bo *bo;
   bool live;
};

struct queue {
   struct context *ctx;
   struct bo *ring;
   struct bo *event;
   uint64_t cookie;
   uint64_t processed;
   uint8_t group;
   void *io;
   uint64_t seqno;
   bool registered;
   bool group_live;
};

static struct context contexts[MAX_CONTEXTS];
static struct bo bos[MAX_BOS];
static struct mapping maps[MAX_MAPS];
static struct queue queues[MAX_QUEUES];
static unsigned context_count, bo_count, map_count, queue_count;
static unsigned kicks, completions, closes, forbidden_calls;
static uint64_t next_cookie = BASE_MEM_COOKIE_BASE;
static bool active;
static unsigned fail_request, fail_occurrence, request_seen;
static unsigned host_allocations, host_live_allocations, fail_host_allocation;
static unsigned logical_map_requests;
static const char *fail_mode;
static bool quarantined;
static struct context *quarantined_context;

static int (*host_open)(const char *, int, ...);
static int (*host_openat)(int, const char *, int, ...);
static int (*host_close)(int);
static void *(*host_mmap)(void *, size_t, int, int, int, off_t);
static int (*host_munmap)(void *, size_t);
static ssize_t (*host_read)(int, void *, size_t);
static int (*host_ppoll)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);

int open(const char *path, int flags, ...);
int open64(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int openat64(int dirfd, const char *path, int flags, ...);
int close(int fd);
int ioctl(int fd, unsigned long request, ...);
void *mmap(void *addr, size_t size, int prot, int flags, int fd, off_t offset);
void *mmap64(void *addr, size_t size, int prot, int flags, int fd, off64_t offset);
int munmap(void *addr, size_t size);
ssize_t read(int fd, void *data, size_t size);
int ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask);

static void
resolve_host(void)
{
   host_open = dlsym(RTLD_NEXT, "open64");
   host_openat = dlsym(RTLD_NEXT, "openat64");
   host_close = dlsym(RTLD_NEXT, "close");
   host_mmap = dlsym(RTLD_NEXT, "mmap64");
   host_munmap = dlsym(RTLD_NEXT, "munmap");
   host_read = dlsym(RTLD_NEXT, "read");
   host_ppoll = dlsym(RTLD_NEXT, "ppoll");
   CHECK(host_open && host_openat && host_close && host_mmap && host_munmap && host_read && host_ppoll);
}

static struct context *
find_context(int fd)
{
   for (unsigned i = 0; i < context_count; i++)
      if (contexts[i].live && contexts[i].fd == fd)
         return &contexts[i];
   return NULL;
}

static bool
fail_ioctl(unsigned long request)
{
   if (_IOC_NR(request) != fail_request)
      return false;
   if (++request_seen != fail_occurrence)
      return false;
   errno = ENOMEM;
   return true;
}

static int
new_context(void)
{
   CHECK(context_count < MAX_CONTEXTS);
   int fd = memfd_create("panvk-host-context", MFD_CLOEXEC);
   CHECK(fd >= 0);
   contexts[context_count] = (struct context){
      .fd = fd, .generation = context_count + 1, .live = true,
   };
   context_count++;
   return fd;
}

static int
mock_open(const char *path, int flags, mode_t mode)
{
   if (!active)
      return host_open(path, flags, mode);
   if (!strcmp(path, "/dev/mali0")) {
      CHECK(flags == (O_RDWR | O_CLOEXEC));
      return new_context();
   }
   const char *prefix = "/proc/self/fd/";
   if (!strncmp(path, prefix, strlen(prefix))) {
      char *end;
      long fd = strtol(path + strlen(prefix), &end, 10);
      CHECK(!*end && fd >= 0 && fd <= INT32_MAX && find_context(fd));
      CHECK(flags == (O_RDWR | O_CLOEXEC));
      if (fail_mode && !strcmp(fail_mode, "reopen")) {
         errno = ENOMEM;
         return -1;
      }
      return new_context();
   }
   if (!strncmp(path, "/dev/", 5)) {
      forbidden_calls++;
      CHECK(false);
   }
   errno = EPERM;
   return -1;
}

int
open(const char *path, int flags, ...)
{
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
   }
   return mock_open(path, flags, mode);
}

int
open64(const char *path, int flags, ...)
{
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
   }
   return mock_open(path, flags, mode);
}

int
openat(int dirfd, const char *path, int flags, ...)
{
   if (active) {
      if (path[0] == '/')
         return mock_open(path, flags, 0);
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
   if (active) {
      if (path[0] == '/')
         return mock_open(path, flags, 0);
      errno = EPERM;
      return -1;
   }
   mode_t mode = 0;
   if (flags & O_CREAT) {
      va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
   }
   return host_openat(dirfd, path, flags, mode);
}

static struct bo *
find_bo_cookie(struct context *ctx, uint64_t cookie)
{
   for (unsigned i = 0; i < bo_count; i++)
      if (bos[i].live && bos[i].ctx == ctx && bos[i].cookie == cookie)
         return &bos[i];
   return NULL;
}

static struct bo *
find_bo_gpu(struct context *ctx, uint64_t gpu, size_t size)
{
   for (unsigned i = 0; i < bo_count; i++) {
      struct bo *bo = &bos[i];
      if (bo->live && bo->ctx == ctx && bo->gpu && gpu >= bo->gpu &&
          gpu - bo->gpu <= bo->size && size <= bo->size - (gpu - bo->gpu))
         return bo;
   }
   return NULL;
}

static void *
bo_cpu(struct bo *bo)
{
   for (unsigned i = 0; i < map_count; i++)
      if (maps[i].live && maps[i].bo == bo)
         return maps[i].ptr;
   CHECK(false);
   return NULL;
}

static struct bo *
new_bo(struct context *ctx, size_t size, uint64_t flags)
{
   CHECK(ctx->generation > 1 && ctx->jit == 1);
   CHECK(bo_count < MAX_BOS && size && !(size & (PAGE - 1)));
   struct bo *bo = &bos[bo_count++];
   bo->ctx = ctx;
   bo->flags = flags;
   bo->size = size;
   bo->live = true;
   bo->cookie = flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP) ? next_cookie : 0x10000000 + bo_count * 0x100000;
   next_cookie += size + PAGE;
   bo->gpu = flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP) ? 0 : bo->cookie;
   bo->backing = memfd_create("panvk-host-bo", MFD_CLOEXEC);
   CHECK(bo->backing >= 0 && ftruncate(bo->backing, size) == 0);
   return bo;
}

static size_t
append_property(uint8_t *buf, size_t offset, unsigned key, uint64_t value, unsigned bytes)
{
   unsigned code = bytes == 8 ? 3 : bytes == 4 ? 2 : bytes == 2 ? 1 : 0;
   uint32_t header = (key << 2) | code;
   memcpy(buf + offset, &header, sizeof(header));
   memcpy(buf + offset + 4, &value, bytes);
   return offset + 4 + bytes;
}

static size_t
make_properties(uint8_t *buf, struct context *ctx)
{
   size_t n = 0;
#define PROP(key, value, bytes) n = append_property(buf, n, key, value, bytes)
   PROP(KBASE_GPUPROP_RAW_GPU_ID, 0xd8300000, 4);
   PROP(KBASE_GPUPROP_RAW_CORE_FEATURES, fail_mode && !strcmp(fail_mode, "profile") && ctx->generation > 1 ? 3 : 4, 4);
   PROP(KBASE_GPUPROP_RAW_GPU_FEATURES, 1ull << 2, 8);
   PROP(KBASE_GPUPROP_RAW_SHADER_PRESENT, 0xfff, 8);
   PROP(KBASE_GPUPROP_RAW_MMU_FEATURES, 48, 4);
   PROP(KBASE_GPUPROP_RAW_TILER_FEATURES, (8 << 8) | 9, 4);
   PROP(KBASE_GPUPROP_RAW_THREAD_MAX_THREADS, 2048, 4);
   PROP(KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE, 1024, 4);
   PROP(KBASE_GPUPROP_RAW_THREAD_FEATURES, (16u << 24) | 32768, 4);
   PROP(KBASE_GPUPROP_TLS_ALLOC, 2048, 4);
   PROP(KBASE_GPUPROP_RAW_L2_FEATURES, 0x06100606, 4);
   PROP(KBASE_GPUPROP_RAW_MEM_FEATURES, 0, 4);
   PROP(KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0, UINT32_MAX, 4);
   PROP(KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1, UINT32_MAX, 4);
   PROP(KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2, UINT32_MAX, 4);
   PROP(KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3, UINT32_MAX, 4);
#undef PROP
   return n;
}

static struct queue *
find_queue(struct context *ctx, uint64_t ring)
{
   for (unsigned i = 0; i < queue_count; i++)
      if (queues[i].ctx == ctx && queues[i].ring->gpu == ring)
         return &queues[i];
   CHECK(false);
   return NULL;
}

static uint64_t
reg64(uint32_t *regs, unsigned reg)
{
   CHECK(reg + 1 < 128);
   return regs[reg] | ((uint64_t)regs[reg + 1] << 32);
}

static void
complete_queue(struct queue *queue)
{
   uint64_t insert = *(uint64_t *)((uint8_t *)queue->io + PAGE + CS_USER_IO_INPUT_CS_INSERT);
   CHECK(insert > queue->processed && insert - queue->processed <= queue->ring->size);
   uint32_t regs[128] = {0};
   unsigned syncs = 0, calls = 0;
   uint64_t seqno_address = 0;
   for (uint64_t pos = queue->processed; pos < insert; pos += 8) {
      uint64_t word;
      memcpy(&word, (uint8_t *)bo_cpu(queue->ring) + pos % queue->ring->size, 8);
      switch (word >> 56) {
      case MALI_CS_OPCODE_NOP:
      case MALI_CS_OPCODE_SET_STATE_IMM32:
      case MALI_CS_OPCODE_REQ_RESOURCE:
      case MALI_CS_OPCODE_HEAP_SET:
      case MALI_CS_OPCODE_FLUSH_CACHE2:
      case MALI_CS_OPCODE_WAIT:
      case MALI_CS_OPCODE_ERROR_BARRIER:
         break;
      case MALI_CS_OPCODE_MOVE32: {
         struct MALI_CS_MOVE32 p;
         MALI_CS_MOVE32_unpack((const struct mali_cs_move32_packed *)&word, &p);
         CHECK(p.destination < 128);
         regs[p.destination] = p.immediate;
         break;
      }
      case MALI_CS_OPCODE_MOVE48: {
         struct MALI_CS_MOVE48 p;
         MALI_CS_MOVE48_unpack((const struct mali_cs_move48_packed *)&word, &p);
         CHECK(p.destination + 1 < 128);
         regs[p.destination] = p.immediate;
         regs[p.destination + 1] = p.immediate >> 32;
         break;
      }
      case MALI_CS_OPCODE_CALL: {
         struct MALI_CS_CALL p;
         MALI_CS_CALL_unpack((const struct mali_cs_call_packed *)&word, &p);
         CHECK(p.length < 128 && regs[p.length] && !(regs[p.length] & 7));
         CHECK(find_bo_gpu(queue->ctx, reg64(regs, p.address), regs[p.length]));
         calls++;
         break;
      }
      case MALI_CS_OPCODE_SYNC_ADD64: {
         struct MALI_CS_SYNC_ADD64 p;
         MALI_CS_SYNC_ADD64_unpack((const struct mali_cs_sync_add64_packed *)&word, &p);
         CHECK(p.error_propagate && p.scope == MALI_CS_SYNC_SCOPE_SYSTEM);
         CHECK(calls == 1 && reg64(regs, p.data) == 1);
         seqno_address = reg64(regs, p.address);
         struct bo *event = find_bo_gpu(queue->ctx, seqno_address, 16);
         CHECK(event && event == queue->event && seqno_address == event->gpu);
         CHECK((event->flags & BASE_MEM_CSF_EVENT) && !(seqno_address & 15));
         syncs++;
         break;
      }
      default:
         fprintf(stderr, "unexpected wrapper opcode %llu\n", (unsigned long long)(word >> 56));
         CHECK(false);
      }
   }
   CHECK(calls == 1 && syncs == 1);
   if (fail_mode && !strcmp(fail_mode, "completion-timeout"))
      return;
   struct bo *event = find_bo_gpu(queue->ctx, seqno_address, 16);
   uint64_t *cell = (void *)((uint8_t *)bo_cpu(event) + seqno_address - event->gpu);
   CHECK(*cell == queue->seqno);
   (*cell)++;
   if (fail_mode && !strcmp(fail_mode, "completion-error"))
      ((uint32_t *)cell)[2] = 1;
   queue->seqno++;
   queue->processed = insert;
   *(uint64_t *)((uint8_t *)queue->io + 2 * PAGE + CS_USER_IO_OUTPUT_CS_EXTRACT) = insert;
   completions++;
}

int
ioctl(int fd, unsigned long request, ...)
{
   va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
   struct context *ctx = find_context(fd);
   if (!active || !ctx) {
      if (active)
         forbidden_calls++;
      errno = EPERM;
      return -1;
   }
   unsigned number = _IOC_NR(request);
   CHECK(_IOC_TYPE(request) == KBASE_IOCTL_TYPE);
   if (number == 45 || number == 46 || number == 47) {
      forbidden_calls++;
      CHECK(false);
   }
   if (fail_ioctl(request))
      return -1;
   if (request == KBASE_IOCTL_VERSION_CHECK_CSF) {
      struct kbase_ioctl_version_check *r = arg;
      CHECK(!r->major && !r->minor);
      r->major = 1; r->minor = 30; return 0;
   }
   if (request == KBASE_IOCTL_SET_FLAGS)
      return 0;
   if (request == KBASE_IOCTL_GET_GPUPROPS) {
      uint8_t props[512]; size_t size = make_properties(props, ctx);
      struct kbase_ioctl_get_gpuprops *r = arg;
      if (!r->buffer && !r->size)
         return size;
      CHECK(r->size == size);
      memcpy((void *)(uintptr_t)r->buffer, props, size);
      return size;
   }
   if (request == KBASE_IOCTL_MEM_JIT_INIT) {
      CHECK(ctx->generation > 1 && !ctx->jit && !ctx->queue_count);
      ctx->jit++;
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_ALLOC_EX) {
      CHECK(ctx->jit == 1);
      union kbase_ioctl_mem_alloc_ex *r = arg;
      CHECK(r->in.va_pages && r->in.commit_pages == r->in.va_pages);
      struct bo *bo = new_bo(ctx, r->in.va_pages * PAGE, r->in.flags);
      r->out.flags = bo->flags;
      r->out.gpu_va = bo->cookie;
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_FREE) {
      struct kbase_ioctl_mem_free *r = arg;
      struct bo *bo = find_bo_cookie(ctx, r->gpu_addr);
      CHECK(bo && bo->live);
      bo->live = false;
      CHECK(host_close(bo->backing) == 0);
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_SYNC) {
      struct kbase_ioctl_mem_sync *r = arg;
      struct bo *bo = find_bo_gpu(ctx, r->handle, r->size);
      CHECK(bo && r->size && (r->type == BASE_SYNCSET_OP_MSYNC || r->type == BASE_SYNCSET_OP_CSYNC));
      CHECK(r->user_addr >= (uintptr_t)bo_cpu(bo));
      CHECK(r->user_addr - (uintptr_t)bo_cpu(bo) + r->size <= bo->size);
      return 0;
   }
   if (request == KBASE_IOCTL_MEM_ALIAS) {
      union kbase_ioctl_mem_alias *r = arg;
      CHECK(r->in.nents == 2 && r->in.stride);
      const struct base_mem_aliasing_info *entries = (const void *)(uintptr_t)r->in.aliasing_info;
      CHECK(entries[0].handle == entries[1].handle && !entries[0].offset && !entries[1].offset);
      CHECK(entries[0].length == r->in.stride && entries[1].length == r->in.stride);
      struct bo *source = find_bo_gpu(ctx, entries[0].handle, r->in.stride * PAGE);
      CHECK(source && source->size == r->in.stride * PAGE);
      struct bo *bo = new_bo(ctx, source->size * 2, r->in.flags | BASE_MEM_NEED_MMAP);
      bo->source = source; bo->repeats = 2;
      r->out.flags = bo->flags; r->out.gpu_va = bo->cookie; r->out.va_pages = bo->size / PAGE;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_GET_GLB_IFACE) {
      union kbase_ioctl_cs_get_glb_iface *r = arg;
      uint32_t groups = 4, streams = 12;
      if (r->in.max_group_num) {
         CHECK(r->in.max_group_num == groups && r->in.max_total_stream_num == streams);
         struct basep_cs_group_control *g = (void *)(uintptr_t)r->in.groups_ptr;
         struct basep_cs_stream_control *s = (void *)(uintptr_t)r->in.streams_ptr;
         for (unsigned i = 0; i < groups; i++)
            g[i] = (struct basep_cs_group_control){.stream_num = 3};
         for (unsigned i = 0; i < streams; i++)
            s[i] = (struct basep_cs_stream_control){.features = 8 << 8};
      }
      *r = (union kbase_ioctl_cs_get_glb_iface){0};
      r->out.group_num = groups; r->out.total_stream_num = streams;
      r->out.glb_version = 0x1001e;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_TILER_HEAP_INIT) {
      union kbase_ioctl_cs_tiler_heap_init *r = arg;
      CHECK(ctx->jit && r->in.chunk_size == 1024 * 1024);
      ctx->heaps_created++;
      ctx->heaps_live++;
      r->out.gpu_heap_va = 0x90000000 + ctx->heaps_created * PAGE;
      r->out.first_chunk_va = 0x90100000 + ctx->heaps_created * 1024 * 1024;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_TILER_HEAP_TERM) {
      CHECK(ctx->heaps_live);
      if (fail_mode && !strcmp(fail_mode, "heap-term")) {
         errno = EIO;
         return -1;
      }
      ctx->heaps_live--;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_REGISTER) {
      struct kbase_ioctl_cs_queue_register *r = arg;
      CHECK(queue_count < MAX_QUEUES && r->buffer_size == 65536);
      struct bo *ring = find_bo_gpu(ctx, r->buffer_gpu_addr, r->buffer_size);
      struct bo *event = NULL;
      for (unsigned i = bo_count; i > 0; i--)
         if (bos[i - 1].ctx == ctx && bos[i - 1].live &&
             (bos[i - 1].flags & BASE_MEM_CSF_EVENT)) {
            event = &bos[i - 1];
            break;
         }
      CHECK(ring && event);
      queues[queue_count++] = (struct queue){.ctx = ctx, .ring = ring, .event = event, .registered = true};
      ctx->queue_count++;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18) {
      union kbase_ioctl_cs_queue_group_create_1_18 *r = arg;
      CHECK(queue_count && queues[queue_count - 1].ctx == ctx);
      struct queue *q = &queues[queue_count - 1];
      q->group = queue_count;
      q->group_live = true;
      r->out.group_handle = q->group;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_BIND) {
      union kbase_ioctl_cs_queue_bind *r = arg;
      struct queue *q = find_queue(ctx, r->in.buffer_gpu_addr);
      CHECK(r->in.group_handle == q->group && !r->in.csi_index);
      q->cookie = next_cookie; next_cookie += 4 * PAGE;
      r->out.mmap_handle = q->cookie;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_KICK) {
      struct kbase_ioctl_cs_queue_kick *r = arg;
      struct queue *q = find_queue(ctx, r->buffer_gpu_addr);
      CHECK(q->registered && q->group_live && q->io);
      for (unsigned i = 0; i < queue_count; i++) {
         if (queues[i].ctx == ctx && queues[i].registered)
            CHECK(*(uint64_t *)((uint8_t *)queues[i].io + PAGE + CS_USER_IO_INPUT_CS_INSERT));
      }
      complete_queue(q);
      kicks++;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_TERMINATE) {
      struct queue *q = find_queue(ctx, ((struct kbase_ioctl_cs_queue_terminate *)arg)->buffer_gpu_addr);
      CHECK(q->registered);
      if (fail_mode && !strcmp(fail_mode, "stop")) {
         errno = EIO;
         return -1;
      }
      q->registered = false; return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE) {
      uint8_t group = ((struct kbase_ioctl_cs_queue_group_term *)arg)->group_handle;
      for (unsigned i = 0; i < queue_count; i++)
         if (queues[i].ctx == ctx && queues[i].group == group && queues[i].group_live) {
            CHECK(!queues[i].registered); queues[i].group_live = false; return 0;
         }
      CHECK(false);
   }
   fprintf(stderr, "unknown ioctl 0x%lx nr%u\n", request, number);
   forbidden_calls++;
   CHECK(false);
   return -1;
}

void *
mmap(void *addr, size_t size, int prot, int flags, int fd, off_t offset)
{
   struct context *ctx = find_context(fd);
   if (!active)
      return host_mmap(addr, size, prot, flags, fd, offset);
   if (!ctx) {
      CHECK(fd == -1 && (flags & MAP_ANONYMOUS));
      return host_mmap(addr, size, prot, flags, fd, offset);
   }
   CHECK(!addr && flags == MAP_SHARED && size && !(size & (PAGE - 1)));
   CHECK(map_count < MAX_MAPS);
   if (ctx->generation > 1 && ++logical_map_requests == 3 &&
       fail_mode && !strcmp(fail_mode, "map")) {
      errno = ENOMEM;
      return MAP_FAILED;
   }
   struct bo *bo = find_bo_cookie(ctx, offset);
   void *ptr = MAP_FAILED;
   if (bo) {
      CHECK(size == bo->size);
      if (!bo->source) {
         ptr = host_mmap(NULL, size, prot, MAP_SHARED, bo->backing, 0);
      } else {
         CHECK(bo->source->live && bo->repeats == 2 && prot == PROT_READ);
         ptr = host_mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         if (ptr != MAP_FAILED) {
            for (unsigned i = 0; i < 2; i++)
               CHECK(host_mmap((uint8_t *)ptr + i * bo->source->size, bo->source->size,
                               PROT_READ, MAP_SHARED | MAP_FIXED, bo->source->backing, 0) != MAP_FAILED);
         }
      }
      if (ptr != MAP_FAILED && (bo->flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP)))
         bo->gpu = (uintptr_t)ptr;
   } else {
      bool special = offset == BASE_MEM_MAP_TRACKING_HANDLE || offset == BASEP_MEM_CSF_USER_REG_PAGE_HANDLE;
      for (unsigned i = 0; i < queue_count; i++)
         special |= queues[i].ctx == ctx && queues[i].cookie == offset;
      CHECK(special);
      ptr = host_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (ptr != MAP_FAILED)
         memset(ptr, 0, size);
      for (unsigned i = 0; i < queue_count; i++)
         if (queues[i].ctx == ctx && queues[i].cookie == offset)
            queues[i].io = ptr;
   }
   CHECK(ptr != MAP_FAILED);
   maps[map_count++] = (struct mapping){.ptr = ptr, .size = size, .ctx = ctx, .bo = bo, .live = true};
   return ptr;
}

void *
mmap64(void *addr, size_t size, int prot, int flags, int fd, off64_t offset)
{
   return mmap(addr, size, prot, flags, fd, offset);
}

int
munmap(void *addr, size_t size)
{
   if (active) {
      for (unsigned i = 0; i < map_count; i++) {
         struct mapping *map = &maps[i];
         if (map->live && map->ptr == addr) {
            CHECK(map->size == size);
            map->live = false;
            if (map->bo && (map->bo->flags & (BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP))) {
               map->bo->live = false;
               CHECK(host_close(map->bo->backing) == 0);
            }
            return host_munmap(addr, size);
         }
      }
   }
   return host_munmap(addr, size);
}

int
close(int fd)
{
   struct context *ctx = find_context(fd);
   if (active && ctx) {
      for (unsigned i = 0; i < queue_count; i++)
         if (queues[i].ctx == ctx)
            CHECK(!queues[i].registered && !queues[i].group_live);
      for (unsigned i = 0; i < bo_count; i++)
         if (bos[i].ctx == ctx)
            CHECK(!bos[i].live);
      ctx->live = false;
      closes++;
   }
   return host_close(fd);
}

ssize_t
read(int fd, void *data, size_t size)
{
   if (active && find_context(fd)) {
      errno = EAGAIN;
      return -1;
   }
   return host_read(fd, data, size);
}

int
ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout, const sigset_t *mask)
{
   if (active && count == 1 && fds) {
      const struct pollfd *input = (const void *)(uintptr_t)fds;
      struct context *ctx = find_context(input->fd);
      CHECK(ctx);
      fds[0].revents = 0;
      if (timeout && (timeout->tv_sec || timeout->tv_nsec))
         nanosleep(timeout, NULL);
      return 0;
   }
   return host_ppoll(fds, count, timeout, mask);
}

struct host_allocation {
   void *address;
   size_t size;
};
static struct host_allocation host_records[4096];

static void *
tracked_allocate(void *data, size_t size, size_t alignment, VkSystemAllocationScope scope)
{
   if (++host_allocations == fail_host_allocation)
      return NULL;
   if (alignment < sizeof(void *))
      alignment = sizeof(void *);
   void *ptr = NULL;
   CHECK(posix_memalign(&ptr, alignment, size) == 0);
   for (unsigned i = 0; i < sizeof(host_records) / sizeof(host_records[0]); i++) {
      if (!host_records[i].address) {
         host_records[i] = (struct host_allocation){ptr, size};
         host_live_allocations++;
         return ptr;
      }
   }
   CHECK(false);
   return NULL;
}

static void
tracked_free(void *data, void *ptr)
{
   if (!ptr)
      return;
   for (unsigned i = 0; i < sizeof(host_records) / sizeof(host_records[0]); i++) {
      if (host_records[i].address == ptr) {
         host_records[i].address = NULL;
         CHECK(host_live_allocations);
         host_live_allocations--;
         free(ptr);
         return;
      }
   }
   CHECK(false);
}

static void *
tracked_reallocate(void *data, void *ptr, size_t size, size_t alignment, VkSystemAllocationScope scope)
{
   if (!ptr)
      return tracked_allocate(data, size, alignment, scope);
   if (!size) {
      tracked_free(data, ptr);
      return NULL;
   }
   for (unsigned i = 0; i < sizeof(host_records) / sizeof(host_records[0]); i++) {
      if (host_records[i].address == ptr) {
         size_t old_size = host_records[i].size;
         void *result = tracked_allocate(data, size, alignment, scope);
         if (result) {
            memcpy(result, ptr, old_size < size ? old_size : size);
            tracked_free(data, ptr);
         }
         return result;
      }
   }
   CHECK(false);
   return NULL;
}

static const VkAllocationCallbacks tracked_allocator = {
   .pfnAllocation = tracked_allocate,
   .pfnReallocation = tracked_reallocate,
   .pfnFree = tracked_free,
};

struct api {
   PFN_vkGetInstanceProcAddr gipa;
   PFN_vkCreateInstance create_instance;
   PFN_vkDestroyInstance destroy_instance;
   PFN_vkEnumeratePhysicalDevices enumerate;
   PFN_vkCreateDevice create_device;
   PFN_vkGetDeviceProcAddr gdpa;
};

static void
check_empty(struct context *ctx)
{
   for (unsigned i = 0; i < bo_count; i++)
      if (bos[i].ctx == ctx)
         CHECK(!bos[i].live);
   for (unsigned i = 0; i < map_count; i++)
      if (maps[i].ctx == ctx)
         CHECK(!maps[i].live);
   CHECK(!ctx->live && !ctx->heaps_live);
}

static VkResult
expected_create_error(void)
{
   CHECK(fail_mode);
   if (!strncmp(fail_mode, "host-oom-", 9))
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!strcmp(fail_mode, "profile"))
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   if (!strcmp(fail_mode, "kick") || !strcmp(fail_mode, "completion-error") ||
       !strcmp(fail_mode, "completion-timeout"))
      return VK_ERROR_DEVICE_LOST;
   if (!strcmp(fail_mode, "allocation") || !strcmp(fail_mode, "heap") ||
       !strcmp(fail_mode, "alias") || !strcmp(fail_mode, "map"))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   return VK_ERROR_INITIALIZATION_FAILED;
}

static void
exercise_public_device(struct api *api, VkPhysicalDevice physical)
{
   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
   VkDeviceCreateInfo create = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info};
   unsigned start_contexts = context_count;
   unsigned live_before = host_live_allocations;
   VkDevice device = VK_NULL_HANDLE;
   VkResult result = api->create_device(physical, &create, &tracked_allocator, &device);
   bool teardown_failure = fail_mode && (!strcmp(fail_mode, "stop") || !strcmp(fail_mode, "heap-term"));
   if (fail_mode && !teardown_failure) {
      fprintf(stderr, "Expected failure case %s returned %d, callbacks=%u\n", fail_mode, result, host_allocations);
      CHECK(result == expected_create_error() && device == VK_NULL_HANDLE);
      CHECK(contexts[0].live && !forbidden_calls);
      for (unsigned i = start_contexts; i < context_count; i++)
         check_empty(&contexts[i]);
      CHECK(host_live_allocations == live_before);
      return;
   }
   fprintf(stderr, "CreateDevice result=%d contexts=%u BOs=%u queues=%u kicks=%u errno=%d\n",
           result, context_count, bo_count, queue_count, kicks, errno);
   CHECK(result == VK_SUCCESS && device != VK_NULL_HANDLE);
   CHECK(context_count == start_contexts + 1);
   struct context *logical = &contexts[context_count - 1];
   CHECK(logical->jit == 1 && logical->queue_count == 3);
   CHECK(kicks >= 3 && completions == kicks);
#define DEVICE_PROC(type, name) type name = (type)api->gdpa(device, "vk" #name); CHECK(name)
   DEVICE_PROC(PFN_vkDestroyDevice, DestroyDevice);
   DEVICE_PROC(PFN_vkGetDeviceQueue, GetDeviceQueue);
   DEVICE_PROC(PFN_vkQueueSubmit, QueueSubmit);
   DEVICE_PROC(PFN_vkQueueWaitIdle, QueueWaitIdle);
   DEVICE_PROC(PFN_vkCreateFence, CreateFence);
   DEVICE_PROC(PFN_vkGetFenceStatus, GetFenceStatus);
   DEVICE_PROC(PFN_vkWaitForFences, WaitForFences);
   DEVICE_PROC(PFN_vkResetFences, ResetFences);
   DEVICE_PROC(PFN_vkDestroyFence, DestroyFence);
   DEVICE_PROC(PFN_vkCreateSemaphore, CreateSemaphore);
   DEVICE_PROC(PFN_vkDestroySemaphore, DestroySemaphore);
   DEVICE_PROC(PFN_vkCreateCommandPool, CreateCommandPool);
   DEVICE_PROC(PFN_vkAllocateCommandBuffers, AllocateCommandBuffers);
   DEVICE_PROC(PFN_vkBeginCommandBuffer, BeginCommandBuffer);
   DEVICE_PROC(PFN_vkEndCommandBuffer, EndCommandBuffer);
   DEVICE_PROC(PFN_vkDestroyCommandPool, DestroyCommandPool);
   VkQueue queue;
   GetDeviceQueue(device, 0, 0, &queue);
   CHECK(queue != VK_NULL_HANDLE);
   VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(CreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS);
   CHECK(GetFenceStatus(device, fence) == VK_NOT_READY);
   CHECK(QueueSubmit(queue, 0, NULL, fence) == VK_SUCCESS);
   CHECK(WaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_SUCCESS);
   CHECK(ResetFences(device, 1, &fence) == VK_SUCCESS);
   VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore semaphore;
   CHECK(CreateSemaphore(device, &semaphore_info, NULL, &semaphore) == VK_SUCCESS);
   VkSubmitInfo producer = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .signalSemaphoreCount = 1, .pSignalSemaphores = &semaphore};
   CHECK(QueueSubmit(queue, 1, &producer, VK_NULL_HANDLE) == VK_SUCCESS);
   VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   VkSubmitInfo consumer = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1, .pWaitSemaphores = &semaphore, .pWaitDstStageMask = &stage};
   CHECK(QueueSubmit(queue, 1, &consumer, fence) == VK_SUCCESS);
   CHECK(GetFenceStatus(device, fence) == VK_SUCCESS);
   CHECK(QueueSubmit(queue, 1, &producer, VK_NULL_HANDLE) == VK_SUCCESS);
   CHECK(QueueSubmit(queue, 1, &consumer, VK_NULL_HANDLE) == VK_SUCCESS);
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS);

   /* Command buffer with zero recorded commands. EndCommandBuffer still emits a
    * non-empty compute stream (wait-slots + CS error save + cache flush), so
    * this submit must issue a real kick -- unlike the commandBufferCount==0
    * submits above, which reach no backend transport call at all. */
   VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = 0};
   VkCommandPool pool;
   CHECK(CreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
   VkCommandBufferAllocateInfo cb_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
   VkCommandBuffer cmd;
   CHECK(AllocateCommandBuffers(device, &cb_info, &cmd) == VK_SUCCESS);
   VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   CHECK(BeginCommandBuffer(cmd, &begin_info) == VK_SUCCESS);
   CHECK(EndCommandBuffer(cmd) == VK_SUCCESS);
   unsigned kicks_before = kicks;
   VkSubmitInfo triv = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1, .pCommandBuffers = &cmd};
   CHECK(QueueSubmit(queue, 1, &triv, fence) == VK_SUCCESS);
   CHECK(WaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_SUCCESS);
   CHECK(ResetFences(device, 1, &fence) == VK_SUCCESS);
   CHECK(kicks == kicks_before + 1);
   CHECK(completions == kicks);
   CHECK(QueueWaitIdle(queue) == VK_SUCCESS);

   DestroyCommandPool(device, pool, NULL);

   DestroySemaphore(device, semaphore, NULL);
   DestroyFence(device, fence, NULL);
   DestroyDevice(device, &tracked_allocator);
   if (teardown_failure) {
      CHECK(logical->live && host_live_allocations > live_before);
      CHECK(logical->heaps_live == 1);
      unsigned live_bos = 0;
      for (unsigned i = 0; i < bo_count; i++)
         live_bos += bos[i].ctx == logical && bos[i].live;
      CHECK(live_bos >= 10);
      for (unsigned i = 0; i < queue_count; i++) {
         if (queues[i].ctx == logical) {
            CHECK(queues[i].ring->live && queues[i].event->live);
            CHECK(queues[i].registered == !strcmp(fail_mode, "stop"));
         }
      }
      quarantined = true;
      quarantined_context = logical;
      return;
   }
   check_empty(logical);
   CHECK(host_live_allocations == live_before);
   CHECK(contexts[0].live);
   DestroyDevice(VK_NULL_HANDLE, NULL);
#undef DEVICE_PROC
}

static int
child_case(const char *library, const char *mode)
{
   resolve_host();
   setenv("PANVK_KBASE_DEVICE", "/dev/mali0", 1);
   setenv("PAN_KMOD_RESTRICT_TO_BACKENDS", "kbase", 1);
   setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
   setenv("MESA_DISK_CACHE_SINGLE_FILE", "false", 1);
   setenv("MESA_VK_ABORT_ON_DEVICE_LOSS", "false", 1);
   void *icd = dlopen(library, RTLD_NOW | RTLD_LOCAL);
   if (!icd)
      fprintf(stderr, "dlopen failed: %s\n", dlerror());
   CHECK(icd);
   struct api api = {0};
   api.gipa = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
   CHECK(api.gipa);
   Dl_info image;
   char expected_path[4096], loaded_path[4096];
   CHECK(dladdr((void *)api.gipa, &image));
   CHECK(realpath(library, expected_path) && realpath(image.dli_fname, loaded_path));
   CHECK(!strcmp(expected_path, loaded_path));
   const struct panvk_icd_binding bindings[] = {
      {"open64", (void *)open64}, {"mmap64", (void *)mmap64},
      {"munmap", (void *)munmap}, {"ioctl", (void *)ioctl},
      {"read", (void *)read}, {"ppoll", (void *)ppoll}, {"close", (void *)close},
   };
   struct panvk_icd_binding wrong_binding = {"open64", (void *)close};
   CHECK(!panvk_icd_verify_bindings((void *)api.gipa, &wrong_binding, 1));
   CHECK(panvk_icd_verify_bindings((void *)api.gipa, bindings, sizeof(bindings) / sizeof(bindings[0])));
   CHECK(panvk_icd_install_syscall_guard());
   errno = 0;
   CHECK(syscall(SYS_openat, AT_FDCWD, "/home/zl/github/mesa/VERSION", O_RDONLY, 0) == -1 && errno == EPERM);
   errno = 0;
   CHECK(syscall(SYS_ioctl, -1, 0, 0) == -1 && errno == EPERM);
   active = true;
   api.create_instance = (PFN_vkCreateInstance)api.gipa(VK_NULL_HANDLE, "vkCreateInstance");
   CHECK(api.create_instance && panvk_icd_symbol_in_image((void *)api.gipa, (void *)api.create_instance));
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
   VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   VkInstance instance;
   CHECK(api.create_instance(&info, NULL, &instance) == VK_SUCCESS);
   api.destroy_instance = (PFN_vkDestroyInstance)api.gipa(instance, "vkDestroyInstance");
   api.enumerate = (PFN_vkEnumeratePhysicalDevices)api.gipa(instance, "vkEnumeratePhysicalDevices");
   api.create_device = (PFN_vkCreateDevice)api.gipa(instance, "vkCreateDevice");
   api.gdpa = (PFN_vkGetDeviceProcAddr)api.gipa(instance, "vkGetDeviceProcAddr");
   CHECK(api.destroy_instance && api.enumerate && api.create_device && api.gdpa);
   uint32_t count = 1;
   VkPhysicalDevice physical;
   CHECK(api.enumerate(instance, &count, &physical) == VK_SUCCESS && count == 1);
   CHECK(context_count == 1 && contexts[0].live && !bo_count);
   fail_mode = strcmp(mode, "success") ? mode : NULL;
   if (fail_mode && !strncmp(mode, "host-oom-", 9)) {
      char *end;
      fail_host_allocation = strtoul(mode + 9, &end, 10);
      CHECK(!*end && fail_host_allocation);
   }
   if (fail_mode && !strcmp(mode, "jit")) { fail_request = 14; fail_occurrence = 1; }
   if (fail_mode && !strcmp(mode, "allocation")) { fail_request = 59; fail_occurrence = 1; }
   if (fail_mode && !strcmp(mode, "register")) { fail_request = 36; fail_occurrence = 2; }
   if (fail_mode && !strcmp(mode, "group")) { fail_request = 58; fail_occurrence = 2; }
   if (fail_mode && !strcmp(mode, "bind")) { fail_request = 39; fail_occurrence = 2; }
   if (fail_mode && !strcmp(mode, "kick")) { fail_request = 37; fail_occurrence = 2; }
   if (fail_mode && !strcmp(mode, "heap")) { fail_request = 48; fail_occurrence = 1; }
   if (fail_mode && !strcmp(mode, "alias")) { fail_request = 21; fail_occurrence = 1; }
   if (fail_mode && !strcmp(mode, "csif")) { fail_request = 51; fail_occurrence = 1; }
   exercise_public_device(&api, physical);
   if (!fail_mode)
      exercise_public_device(&api, physical);
   api.destroy_instance(instance, NULL);
   check_empty(&contexts[0]);
   CHECK(!forbidden_calls);
   CHECK(closes == context_count - (quarantined ? 1 : 0));
   if (quarantined)
      CHECK(quarantined_context && quarantined_context->live && host_live_allocations);
   fprintf(stderr, "ICD host case %s passed: contexts=%u queues=%u completions=%u quarantine=%u\n", mode, context_count, queue_count, completions, quarantined);
   return 0;
}

int
main(int argc, char **argv)
{
   CHECK(argc == 3);
   pid_t child = fork();
   CHECK(child >= 0);
   if (!child)
      _exit(child_case(argv[1], argv[2]));
   int status = 0;
   for (unsigned tick = 0; tick < 400; tick++) {
      pid_t result = waitpid(child, &status, WNOHANG);
      CHECK(result >= 0);
      if (result == child)
         return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
      struct timespec delay = {.tv_nsec = 50000000};
      nanosleep(&delay, NULL);
   }
   kill(child, SIGKILL);
   CHECK(waitpid(child, &status, 0) == child);
   fprintf(stderr, "ICD host case timed out\n");
   return 1;
}
