#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "drm-uapi/mali_kbase_ioctl.h"
#include "drm-uapi/panthor_drm.h"
#include "kbase_csf.h"
#include "kbase_kmod.h"
#include "pan_kmod.h"
#include "util/os_time.h"

int __wrap_ioctl(int fd, unsigned long request, ...);
void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
void *__wrap_mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset);
int __wrap_munmap(void *addr, size_t length);
int __wrap_close(int fd);
int __wrap_ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout,
                 const sigset_t *sigmask);
ssize_t __wrap_read(int fd, void *buf, size_t count);
drmVersionPtr __wrap_drmGetVersion(int fd);

_Static_assert(sizeof(union kbase_ioctl_cs_get_glb_iface) == 24, "CSIF ABI");
_Static_assert(sizeof(union kbase_ioctl_cs_queue_group_create_1_18) == 40, "CSG ABI");
_Static_assert(KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18 == 0xc028803aUL, "CSG ioctl");
_Static_assert(KBASE_IOCTL_CS_QUEUE_REGISTER == 0x40108024UL, "register ioctl");
_Static_assert(KBASE_IOCTL_CS_QUEUE_BIND == 0xc0108027UL, "bind ioctl");
_Static_assert(sizeof(struct base_csf_notification) == 64, "notification ABI");

#define FAKE_FD 123
#define PAGE_SIZE 4096
#define MAX_MAPPINGS 32
#define MAX_ALLOCS 16

static void
check_at(bool condition, const char *expression, unsigned line)
{
   if (!condition) {
      fprintf(stderr, "kbase CSF mock test failure at line %u: %s\n", line, expression);
      abort();
   }
}
#define check(condition) check_at((condition), #condition, __LINE__)

struct fake_mapping { void *address; size_t size; off_t offset; bool live; };
struct fake_alloc { uint64_t cookie, flags; bool live; };
static struct fake_mapping mappings[MAX_MAPPINGS];
static struct fake_alloc allocs[MAX_ALLOCS];
static unsigned mapping_count, mmap_calls, munmap_calls, close_calls, drm_version_calls;
static unsigned ioctl_calls, alloc_calls, free_calls, bind_calls, register_calls;
static unsigned queue_term_calls, group_term_calls, sync_calls, kick_calls;
static unsigned poll_calls, read_calls, unexpected_calls;
static uint64_t next_cookie;
static uint8_t props[128];
static size_t props_size;
static uint32_t glb_version = 0x00010019;
static uint32_t group_count = 2, stream_count = 4;
static uint32_t raw_gpu_id = 0xd8300000;
static uint32_t raw_variant = 4;
static uint8_t group_handle;
static uint16_t uapi_minor = 30;
static uint64_t last_bind_cookie;
static uint64_t group_request;
static bool changed_counts;
static bool fail_mmap_io, fail_mmap_reg, fail_bind, fail_register, fail_sync;
static bool fail_group_create, fail_group_term, fail_queue_term, fail_kick;
static bool poll_eintr, poll_hup, short_read, read_eintr, read_zero;
static int poll_ready = 1;
static uint8_t notification[sizeof(struct base_csf_notification)];
static unsigned notification_size = sizeof(struct base_csf_notification);
static void *user_io_base, *user_reg_base;
static void *user_ios[8];
static unsigned user_io_count;
static bool check_batch_inserts;
static unsigned call_log[4096], call_log_count;
enum { LOG_SYNC = 1, LOG_KICK = 2, LOG_QUEUE_TERM = 3, LOG_GROUP_TERM = 4 };

static unsigned kcpu_create_calls, kcpu_delete_calls, kcpu_enqueue_calls;
static bool fail_kcpu_create, fail_kcpu_enqueue;
static uint8_t kcpu_id = 3;
static uint8_t last_kcpu_type;
static uint64_t last_cqs_addr;
static uint32_t last_cqs_val;
static int kcpu_fence_fd = 99;

static void
put_le(uint8_t *dst, uint64_t value, unsigned size)
{
   for (unsigned i = 0; i < size; i++) dst[i] = value >> (i * 8);
}
static size_t
append_prop(size_t off, uint32_t key, unsigned size, uint64_t value)
{
   uint32_t header = (key << 2) | (size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3);
   put_le(props + off, header, 4);
   put_le(props + off + 4, value, size);
   return off + 4 + size;
}
static void
make_props(void)
{
   props_size = append_prop(0, KBASE_GPUPROP_RAW_GPU_ID, 4, raw_gpu_id);
   props_size = append_prop(props_size, KBASE_GPUPROP_RAW_CORE_FEATURES, 4,
                            raw_variant);
   props_size = append_prop(props_size, KBASE_GPUPROP_RAW_SHADER_PRESENT, 8, 0xfff);
   props_size = append_prop(props_size, KBASE_GPUPROP_RAW_MMU_FEATURES, 4, 48);
   props_size = append_prop(props_size, KBASE_GPUPROP_RAW_GPU_FEATURES, 8,
                            0x112233445566778cull);
   props_size = append_prop(props_size, KBASE_GPUPROP_RAW_L2_FEATURES, 4, 0xaabbccdd);
}
static void
reset_fake(void)
{
   for (unsigned i = 0; i < mapping_count; i++)
      if (mappings[i].live) free(mappings[i].address);
   memset(mappings, 0, sizeof(mappings));
   memset(allocs, 0, sizeof(allocs));
   mapping_count = mmap_calls = munmap_calls = close_calls = drm_version_calls = 0;
   ioctl_calls = alloc_calls = free_calls = bind_calls = register_calls = 0;
   queue_term_calls = group_term_calls = sync_calls = kick_calls = 0;
   poll_calls = read_calls = unexpected_calls = 0;
   next_cookie = BASE_MEM_COOKIE_BASE;
   glb_version = 0x00010019; group_count = 2; stream_count = 4;
   raw_gpu_id = 0xd8300000; raw_variant = 4; group_handle = 0;
   uapi_minor = 30; last_bind_cookie = 0; group_request = 0;
   changed_counts = false;
   fail_mmap_io = fail_mmap_reg = fail_bind = fail_register = fail_sync = false;
   fail_group_create = fail_group_term = fail_queue_term = fail_kick = false;
   poll_eintr = poll_hup = short_read = read_eintr = read_zero = false;
   poll_ready = 1; memset(notification, 0, sizeof(notification));
   notification_size = sizeof(struct base_csf_notification);
   user_io_base = user_reg_base = NULL;
   memset(user_ios, 0, sizeof(user_ios));
   user_io_count = 0; check_batch_inserts = false;
   call_log_count = 0; make_props();
   kcpu_create_calls = kcpu_delete_calls = kcpu_enqueue_calls = 0;
   fail_kcpu_create = fail_kcpu_enqueue = false;
   kcpu_id = 3; last_kcpu_type = 0; last_cqs_addr = 0; last_cqs_val = 0;
   kcpu_fence_fd = 99;
}
static void *test_zalloc(const struct pan_kmod_allocator *a, size_t s, bool t) { return calloc(1, s); }
static void test_free(const struct pan_kmod_allocator *a, void *p) { free(p); }
static const struct pan_kmod_allocator allocator = {.zalloc = test_zalloc, .free = test_free};
static struct pan_kmod_dev *create_device(void)
{
   return kbase_kmod_ops.dev_create(FAKE_FD, PAN_KMOD_DEV_FLAG_OWNS_FD, &allocator);
}
static struct fake_alloc *find_alloc(uint64_t cookie)
{
   for (unsigned i = 0; i < MAX_ALLOCS; i++)
      if (allocs[i].live && allocs[i].cookie == cookie) return &allocs[i];
   return NULL;
}

int
__wrap_ioctl(int fd, unsigned long request, ...)
{
   va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
   check(fd == FAKE_FD); ioctl_calls++;
   if (request == KBASE_IOCTL_VERSION_CHECK_CSF) {
      struct kbase_ioctl_version_check *v = arg; v->major = 1; v->minor = uapi_minor; return 0;
   }
   if (request == KBASE_IOCTL_VERSION_CHECK_JM) { errno = ENOTTY; return -1; }
   if (request == KBASE_IOCTL_SET_FLAGS) return 0;
   if (request == KBASE_IOCTL_GET_GPUPROPS) {
      struct kbase_ioctl_get_gpuprops *r = arg;
      if (!r->buffer && !r->size) return props_size;
      check(r->size == props_size); memcpy((void *)(uintptr_t)r->buffer, props, props_size);
      return props_size;
   }
   if (request == KBASE_IOCTL_MEM_ALLOC_EX || request == KBASE_IOCTL_MEM_ALLOC) {
      uint64_t pages, flags;
      if (request == KBASE_IOCTL_MEM_ALLOC_EX) {
         union kbase_ioctl_mem_alloc_ex *r = arg;
         pages = r->in.va_pages; flags = r->in.flags;
         check(pages && r->in.commit_pages == pages);
         r->out.flags = flags;
         r->out.gpu_va = (flags & BASE_MEM_SAME_VA) ? next_cookie : 0x80000000ull + alloc_calls * PAGE_SIZE;
      } else {
         union kbase_ioctl_mem_alloc *r = arg;
         pages = r->in.va_pages; flags = r->in.flags;
         check(pages && r->in.commit_pages == pages);
         r->out.flags = flags;
         r->out.gpu_va = (flags & BASE_MEM_SAME_VA) ? next_cookie : 0x80000000ull + alloc_calls * PAGE_SIZE;
      }
      check(alloc_calls < MAX_ALLOCS); uint64_t cookie = (flags & BASE_MEM_SAME_VA) ? next_cookie : 0x80000000ull + (alloc_calls + 1) * PAGE_SIZE;
      if (request == KBASE_IOCTL_MEM_ALLOC_EX) ((union kbase_ioctl_mem_alloc_ex *)arg)->out.gpu_va = cookie;
      else ((union kbase_ioctl_mem_alloc *)arg)->out.gpu_va = cookie;
      allocs[alloc_calls++] = (struct fake_alloc){cookie, flags, true};
      next_cookie += pages * PAGE_SIZE; return 0;
   }
   if (request == KBASE_IOCTL_MEM_FREE) {
      struct fake_alloc *a = find_alloc(((struct kbase_ioctl_mem_free *)arg)->gpu_addr);
      check(a); a->live = false; free_calls++; return 0;
   }
   if (request == KBASE_IOCTL_MEM_SYNC) {
      struct kbase_ioctl_mem_sync *r = arg; check(r->handle && r->size);
      sync_calls++; call_log[call_log_count++] = LOG_SYNC;
      if (fail_sync) { errno = EIO; return -1; } return 0;
   }
   if (request == KBASE_IOCTL_CS_GET_GLB_IFACE) {
      union kbase_ioctl_cs_get_glb_iface *r = arg;
      if (!r->in.max_group_num && !r->in.max_total_stream_num) {
         r->out.group_num = group_count; r->out.total_stream_num = stream_count;
         r->out.glb_version = glb_version; r->out.features = 0x1234;
         r->out.instr_features = 0x5678; return 0;
      }
      check(r->in.max_group_num == group_count && r->in.max_total_stream_num == stream_count);
      struct basep_cs_group_control *g = (void *)(uintptr_t)r->in.groups_ptr;
      struct basep_cs_stream_control *s = (void *)(uintptr_t)r->in.streams_ptr;
      check(g && s);
      for (uint32_t i = 0; i < group_count; i++) g[i] = (struct basep_cs_group_control){.features = 0x100 + i, .stream_num = 2};
      for (uint32_t i = 0; i < stream_count; i++)
      s[i] = (struct basep_cs_stream_control){.features = 0x400};
      *r = (union kbase_ioctl_cs_get_glb_iface){0};
      r->out.group_num = group_count + (changed_counts ? 1 : 0);
      r->out.total_stream_num = stream_count;
      r->out.glb_version = glb_version;
      r->out.features = 0x1234;
      r->out.instr_features = 0x5678;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_REGISTER) {
      struct kbase_ioctl_cs_queue_register *r = arg;
      check(r->buffer_gpu_addr && r->buffer_size >= PAGE_SIZE && r->priority <= 3);
      check(!r->padding[0] && !r->padding[1] && !r->padding[2]); register_calls++;
      if (fail_register) { errno = ENOMEM; return -1; } return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6 || request == KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18 || request == KBASE_IOCTL_CS_QUEUE_GROUP_CREATE) {
      group_request = request;
      check(request != KBASE_IOCTL_CS_QUEUE_GROUP_CREATE);
      if (fail_group_create) { errno = ENOMEM; return -1; }
      if (request == KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6) ((union kbase_ioctl_cs_queue_group_create_1_6 *)arg)->out.group_handle = group_handle;
      else if (request == KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_18) ((union kbase_ioctl_cs_queue_group_create_1_18 *)arg)->out.group_handle = group_handle;
      else ((union kbase_ioctl_cs_queue_group_create *)arg)->out.group_handle = group_handle;
      return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE) {
      struct kbase_ioctl_cs_queue_group_term *r = arg; check(r->group_handle == group_handle);
      group_term_calls++; call_log[call_log_count++] = LOG_GROUP_TERM;
      if (fail_group_term) { errno = EIO; return -1; } return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_BIND) {
      union kbase_ioctl_cs_queue_bind *r = arg;
      check(r->in.buffer_gpu_addr && r->in.group_handle == group_handle && r->in.csi_index < 2);
      bind_calls++; if (fail_bind) { errno = EIO; return -1; }
      last_bind_cookie = next_cookie;
      r->out.mmap_handle = next_cookie; next_cookie += 3 * PAGE_SIZE; return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_TERMINATE) {
      struct kbase_ioctl_cs_queue_terminate *r = arg; check(r->buffer_gpu_addr);
      queue_term_calls++; call_log[call_log_count++] = LOG_QUEUE_TERM;
      if (fail_queue_term) { errno = EIO; return -1; } return 0;
   }
   if (request == KBASE_IOCTL_CS_QUEUE_KICK) {
      struct kbase_ioctl_cs_queue_kick *r = arg; check(r->buffer_gpu_addr);
      if (check_batch_inserts) {
         for (unsigned i = 0; i < user_io_count; i++)
            check(*(uint64_t *)((uint8_t *)user_ios[i] + PAGE_SIZE) != 0);
      }
      kick_calls++; call_log[call_log_count++] = LOG_KICK;
      if (fail_kick) { errno = EIO; return -1; } return 0;
   }
   if (request == KBASE_IOCTL_KCPU_QUEUE_CREATE) {
      struct kbase_ioctl_kcpu_queue_new *r = arg;
      kcpu_create_calls++;
      if (fail_kcpu_create) { errno = ENOMEM; return -1; }
      r->id = kcpu_id; return 0;
   }
   if (request == KBASE_IOCTL_KCPU_QUEUE_DELETE) {
      struct kbase_ioctl_kcpu_queue_delete *r = arg;
      kcpu_delete_calls++;
      check(r->id == kcpu_id);
      return 0;
   }
   if (request == KBASE_IOCTL_KCPU_QUEUE_ENQUEUE) {
      struct kbase_ioctl_kcpu_queue_enqueue *r = arg;
      kcpu_enqueue_calls++;
      check(r->id == kcpu_id && r->nr_commands == 1 && r->addr);
      struct base_kcpu_command *cmd = (void *)(uintptr_t)r->addr;
      last_kcpu_type = cmd->type;
      if (cmd->type == BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL) {
         struct base_fence *f = (void *)(uintptr_t)cmd->info.fence.fence;
         check(f);
         f->basep.fd = kcpu_fence_fd; f->basep.stream_fd = -1;
      } else if (cmd->type == BASE_KCPU_COMMAND_TYPE_CQS_WAIT) {
         struct base_cqs_wait_info *w = (void *)(uintptr_t)cmd->info.cqs_wait.objs;
         check(w && cmd->info.cqs_wait.nr_objs == 1);
         last_cqs_addr = w->addr; last_cqs_val = w->val;
      } else {
         check(!"unexpected kcpu command type");
      }
      if (fail_kcpu_enqueue) { errno = EINVAL; return -1; }
      return 0;
   }
   unexpected_calls++; errno = ENOTTY; return -1;
}

void *
__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
   check(fd == FAKE_FD); mmap_calls++; check(length && flags == MAP_SHARED);
   if (offset == BASE_MEM_MAP_TRACKING_HANDLE) check(length == PAGE_SIZE && prot == PROT_NONE);
   else if (offset == last_bind_cookie && last_bind_cookie != 0) {
      check(length == PAGE_SIZE * 3 && prot == (PROT_READ | PROT_WRITE));
      if (fail_mmap_io) { errno = ENOMEM; return MAP_FAILED; }
   } else if (offset == BASEP_MEM_CSF_USER_REG_PAGE_HANDLE) {
      check(length == PAGE_SIZE && prot == PROT_READ);
      if (fail_mmap_reg) { errno = ENOMEM; return MAP_FAILED; }
   } else {
      struct fake_alloc *a = find_alloc(offset); check(a && prot == (PROT_READ | PROT_WRITE));
   }
   check(mapping_count < MAX_MAPPINGS); size_t rounded = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
   void *p = aligned_alloc(PAGE_SIZE, rounded); if (!p) { errno = ENOMEM; return MAP_FAILED; }
   memset(p, 0, rounded); mappings[mapping_count++] = (struct fake_mapping){p, length, offset, true};
   if (offset == last_bind_cookie && last_bind_cookie != 0) {
      user_io_base = p;
      check(user_io_count < ARRAY_SIZE(user_ios));
      user_ios[user_io_count++] = p;
   }
   if (offset == BASEP_MEM_CSF_USER_REG_PAGE_HANDLE) user_reg_base = p;
   return p;
}
void *__wrap_mmap64(void *a, size_t n, int p, int f, int fd, off64_t o) { return __wrap_mmap(a,n,p,f,fd,o); }
int
__wrap_munmap(void *addr, size_t length)
{
   munmap_calls++;
   for (unsigned i = 0; i < mapping_count; i++) if (mappings[i].address == addr && mappings[i].live) {
      check(length == mappings[i].size); mappings[i].live = false; free(addr); return 0;
   }
   unexpected_calls++; errno = EINVAL; return -1;
}
int __wrap_close(int fd) { check(fd == FAKE_FD); close_calls++; return 0; }
drmVersionPtr __wrap_drmGetVersion(int fd) { check(fd == FAKE_FD); drm_version_calls++; return NULL; }
int
__wrap_ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *sigmask)
{
   check(call_log_count < ARRAY_SIZE(call_log));
   poll_calls++; check(nfds == 1 && fds[0].fd == FAKE_FD);
   if (!poll_eintr && timeout)
      nanosleep(timeout, NULL);
   if (poll_eintr) { poll_eintr = false; errno = EINTR; return -1; }
   if (poll_hup) { fds[0].revents = POLLHUP; return 1; }
   if (poll_ready) { fds[0].revents = POLLIN; return 1; }
   return 0;
}
ssize_t
__wrap_read(int fd, void *buf, size_t count)
{
   check(fd == FAKE_FD && count == sizeof(struct base_csf_notification)); read_calls++;
   if (read_eintr) { errno = EINTR; return -1; }
   if (read_zero) return 0;
   size_t n = short_read ? count - 1 : notification_size; if (n > count) n = count;
   memcpy(buf, notification, n); return n;
}

static struct kbase_csf_context *create_context(struct pan_kmod_dev **out)
{
   struct pan_kmod_dev *dev = create_device(); check(dev); *out = dev;
   return kbase_csf_context_create(dev);
}
static void finish_context(struct kbase_csf_context *ctx, struct pan_kmod_dev *dev)
{
   kbase_csf_context_unref(ctx);
   for (unsigned i = 0; i < mapping_count; i++) check(!mappings[i].live);
   check(unexpected_calls == 0);
}

static void
test_context_caps(void)
{
   reset_fake(); struct pan_kmod_dev *dev = NULL; struct kbase_csf_context *ctx = create_context(&dev); check(ctx);
   const struct kbase_csf_caps *caps = kbase_csf_context_caps(ctx);
   check(caps && caps->version == glb_version && caps->group_count == group_count);
   check(caps->streams_per_group == 2 && caps->stream_features && caps->instr_features == 0x5678);
   check(kbase_csf_context_dev(ctx) == dev && kbase_csf_context_vm(ctx) != NULL);
   struct drm_panthor_csif_info csif = {0};
   check(kbase_csf_context_get_csif(ctx, &csif) == 0);
   check(csif.csg_slot_count == group_count && csif.cs_slot_count == 2 &&
         csif.cs_reg_count == 128 && csif.scoreboard_slot_count == 4 &&
         csif.unpreserved_cs_reg_count == 4);
   check(kbase_csf_context_status(ctx) == 0);
   uint32_t flush = 0xabcddcba; memcpy(user_reg_base, &flush, 4); check(kbase_csf_context_flush_id(ctx) == flush);
   finish_context(ctx, dev);
   reset_fake(); raw_variant = 3; make_props(); dev = NULL; ctx = create_context(&dev); check(ctx);
   check(kbase_csf_context_get_csif(ctx, &csif) == -1 && errno == ENOTSUP);
   finish_context(ctx, dev);
   reset_fake(); uapi_minor = 31; dev = NULL; ctx = create_context(&dev); check(ctx);
   check(kbase_csf_context_get_csif(ctx, &csif) == -1 && errno == ENOTSUP);
   finish_context(ctx, dev);
   reset_fake(); poll_ready = 1; ((struct base_csf_notification *)notification)->type = BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR;
   dev = NULL; ctx = create_context(&dev); check(ctx);
   check(kbase_csf_context_status(ctx) == -1 && errno == EIO && read_calls == 1);
   finish_context(ctx, dev);
   reset_fake(); group_count = 257; dev = create_device(); check(dev);
   check(!kbase_csf_context_create(dev) && errno == EPROTO && close_calls == 0); kbase_kmod_ops.dev_destroy(dev);
   for (unsigned i = 0; i < mapping_count; i++) check(!mappings[i].live);
   reset_fake(); group_count = 0; dev = create_device(); check(dev);
   check(!kbase_csf_context_create(dev) && errno == EPROTO); kbase_kmod_ops.dev_destroy(dev);
   reset_fake(); stream_count = 65537; dev = create_device(); check(dev);
   check(!kbase_csf_context_create(dev) && errno == EPROTO); kbase_kmod_ops.dev_destroy(dev);
   reset_fake(); changed_counts = true; dev = create_device(); check(dev);
   check(!kbase_csf_context_create(dev) && errno == EPROTO && close_calls == 0);
   kbase_kmod_ops.dev_destroy(dev);
}
static void
test_queue_failures(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = create_device();
   check(dev);
   fail_mmap_reg = true;
   check(!kbase_csf_context_create(dev) && errno == ENOMEM && close_calls == 0);
   kbase_kmod_ops.dev_destroy(dev);

   reset_fake();
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   fail_register = true;
   check(!kbase_csf_queue_create(ctx, PAGE_SIZE, 1) && errno == ENOMEM);
   check(register_calls == 1 && queue_term_calls == 0);
   finish_context(ctx, dev);

   reset_fake(); ctx = create_context(&dev); check(ctx); fail_bind = true;
   check(!kbase_csf_queue_create(ctx, PAGE_SIZE, 1) && errno == EIO);
   check(register_calls == 1 && bind_calls == 1 && queue_term_calls == 1);
   finish_context(ctx, dev);

   reset_fake(); ctx = create_context(&dev); check(ctx); fail_mmap_io = true;
   check(!kbase_csf_queue_create(ctx, PAGE_SIZE, 1) && errno == ENOMEM);
   check(register_calls == 1 && bind_calls == 1 && queue_term_calls == 1);
   finish_context(ctx, dev);

   reset_fake(); ctx = create_context(&dev); check(ctx);
   check(!kbase_csf_queue_create(ctx, PAGE_SIZE, 4) && errno == EINVAL);
   check(!kbase_csf_queue_create(ctx, PAGE_SIZE - 1, 0) && errno == EINVAL);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 0); check(q);
   check(kbase_csf_queue_stop(q) == 0 && queue_term_calls == 1);
   check(kbase_csf_queue_stop(q) == 0 && queue_term_calls == 1);
   check(kbase_csf_queue_prepare(q, &(uint64_t){0}, sizeof(uint64_t), 1) == -1);
   kbase_csf_queue_unref(q); finish_context(ctx, dev);
}
static void
test_legacy_group_zero_handle(void)
{
   reset_fake(); uapi_minor = 17; group_handle = 0;
   struct pan_kmod_dev *dev = NULL; struct kbase_csf_context *ctx = create_context(&dev); check(ctx);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 3); check(q);
   kbase_csf_queue_unref(q); finish_context(ctx, dev);
   check(queue_term_calls == 1 && group_term_calls == 1);
   check(group_request == KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6);
}
static void
test_prepare_publish_wait(void)
{
   reset_fake(); struct pan_kmod_dev *dev = NULL; struct kbase_csf_context *ctx = create_context(&dev); check(ctx);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 1); check(q && user_io_base);
   uint8_t command[8] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
   check(kbase_csf_queue_prepare(q, command, 8, 1) == 0);
   check(kbase_csf_queue_published_target(q) == 0);
   check(kbase_csf_queue_prepare(q, command, 8, 1) == -1 && errno == EINVAL);
   check(kbase_csf_queue_prepare(q, command, 7, 2) == -1 && errno == EINVAL);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == 0);
   uint64_t target = kbase_csf_queue_published_target(q); check(target == 1 && kick_calls == 1);
   uint64_t insert = *(uint64_t *)((char *)user_io_base + PAGE_SIZE + CS_USER_IO_INPUT_CS_INSERT);
   check(insert == 64);
   check(kbase_csf_queue_prepare(q, command, 8, 2) == 0);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == 0);
   *(uint64_t *)(uintptr_t)kbase_csf_queue_seqno_va(q) = 0;
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q},1,(uint64_t[]){2},0) == 0);
   *(uint64_t *)(uintptr_t)kbase_csf_queue_seqno_va(q) = 2;
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q},1,(uint64_t[]){2},0) == 1);
   kbase_csf_queue_unref(q); finish_context(ctx, dev);
}
static void
test_publish_failure_cancel_event(void)
{
   reset_fake(); struct pan_kmod_dev *dev = NULL; struct kbase_csf_context *ctx = create_context(&dev); check(ctx);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 0); check(q);
   uint8_t command[8] = {0}; check(kbase_csf_queue_prepare(q, command, 8, 1) == 0);
   fail_kick = true; check(kbase_csf_publish((struct kbase_csf_queue *const[]){q},1) == -1 && errno == EIO);
   fail_kick = false; check(kbase_csf_publish((struct kbase_csf_queue *const[]){q},1) == -1);
   kbase_csf_queue_cancel(q);
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q},1,(uint64_t[]){0},0) == -1);
   kbase_csf_queue_unref(q); finish_context(ctx, dev);

   reset_fake(); ctx = create_context(&dev); check(ctx); q = kbase_csf_queue_create(ctx, PAGE_SIZE, 0); check(q);
   check(kbase_csf_queue_prepare(q, command, 8, 1) == 0);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q},1) == 0);
   ((struct base_csf_notification *)notification)->type = BASE_CSF_NOTIFICATION_EVENT;
   poll_ready = 1;
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q},1,(uint64_t[]){1},
                        os_time_get_nano() + 1000000ull) == 0);
   check(poll_calls >= 1 && read_calls >= 1);
   poll_eintr = true;
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q},1,(uint64_t[]){1},
                        os_time_get_nano() + 1000000ull) == 0);
   poll_eintr = false; short_read = true;
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q},1,(uint64_t[]){1},UINT64_MAX) == -1);
   short_read = false; poll_hup = true;
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q},1,(uint64_t[]){1},UINT64_MAX) == -1);
   kbase_csf_queue_unref(q); finish_context(ctx, dev);
}
static void
test_ring_backpressure_wrap_and_batch(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   check(q);
   uint64_t command = 0x1122334455667788ull;
   for (uint64_t i = 1; i <= 64; i++)
      check(kbase_csf_queue_prepare(q, &command, sizeof(command), i) == 0);
   check(kbase_csf_queue_prepare(q, &command, sizeof(command), 65) == -1 &&
         errno == EAGAIN);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == 0);
   check(*(uint64_t *)((uint8_t *)user_io_base + PAGE_SIZE) == PAGE_SIZE);
   *(uint64_t *)((uint8_t *)user_io_base + 2 * PAGE_SIZE) = PAGE_SIZE;
   check(kbase_csf_queue_prepare(q, &command, sizeof(command), 65) == 0);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == 0);
   check(*(uint64_t *)((uint8_t *)user_io_base + PAGE_SIZE) == PAGE_SIZE + 64);
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q}, 1,
                         (uint64_t[]){65}, 0) == 0);
   *(uint64_t *)(uintptr_t)kbase_csf_queue_seqno_va(q) = 65;
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q}, 1,
                         (uint64_t[]){65}, 0) == 1);
   kbase_csf_queue_unref(q);
   finish_context(ctx, dev);

   reset_fake(); ctx = create_context(&dev); check(ctx);
   struct kbase_csf_queue *a = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   struct kbase_csf_queue *b = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   check(a && b);
   check(kbase_csf_queue_prepare(a, &command, sizeof(command), 1) == 0);
   check(kbase_csf_queue_prepare(b, &command, sizeof(command), 1) == 0);
   struct kbase_csf_queue *batch[] = {a, b};
   check_batch_inserts = true;
   call_log_count = 0;
   check(kbase_csf_publish(batch, 2) == 0);
   check(call_log_count == 4 && call_log[0] == LOG_SYNC &&
         call_log[1] == LOG_SYNC && call_log[2] == LOG_KICK &&
         call_log[3] == LOG_KICK);
   kbase_csf_queue_unref(a);
   kbase_csf_queue_unref(b);
   finish_context(ctx, dev);
}

static void
test_atomic_submit_rollback(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   struct kbase_csf_queue *a = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   struct kbase_csf_queue *b = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   check(a && b);
   uint64_t command = 0x1122334455667788ull;
   for (uint64_t target = 1; target <= 64; target++)
      check(kbase_csf_queue_prepare(b, &command, sizeof(command), target) == 0);

   struct kbase_csf_submit_entry entries[] = {
      {a, &command, sizeof(command), 1},
      {b, &command, sizeof(command), 65},
   };
   check(kbase_csf_submit(entries, ARRAY_SIZE(entries)) == -1 && errno == EAGAIN);
   check(kbase_csf_queue_published_target(a) == 0 &&
         kbase_csf_queue_published_target(b) == 0 && kick_calls == 0);
   check(kbase_csf_queue_prepare(a, &command, sizeof(command), 1) == 0);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){a}, 1) == 0);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){b}, 1) == 0);
   check(kbase_csf_queue_published_target(a) == 1 &&
         kbase_csf_queue_published_target(b) == 64);
   kbase_csf_queue_unref(a);
   kbase_csf_queue_unref(b);
   finish_context(ctx, dev);
}

static void
test_atomic_submit_cache_and_kick_failures(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   check(q);
   uint64_t command = 0;
   struct kbase_csf_submit_entry entry = {q, &command, sizeof(command), 1};
   fail_sync = true;
   check(kbase_csf_submit(&entry, 1) == -1 && errno == EIO);
   check(kbase_csf_queue_published_target(q) == 0 && kick_calls == 0);
   fail_sync = false;
   check(kbase_csf_submit(&entry, 1) == 0);
   check(kbase_csf_queue_published_target(q) == 1);
   kbase_csf_queue_unref(q);
   finish_context(ctx, dev);

   reset_fake(); ctx = create_context(&dev); check(ctx);
   q = kbase_csf_queue_create(ctx, PAGE_SIZE, 1); check(q);
   entry = (struct kbase_csf_submit_entry){q, &command, sizeof(command), 1};
   fail_kick = true;
   check(kbase_csf_submit(&entry, 1) == -1 && errno == EIO);
   check(kbase_csf_queue_published_target(q) == 1);
   fail_kick = false;
   check(kbase_csf_queue_prepare(q, &command, sizeof(command), 2) == -1);
   kbase_csf_queue_unref(q);
   finish_context(ctx, dev);
}

static void
test_atomic_submit_stale_target(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   check(q);
   uint64_t command = 0;
   struct kbase_csf_submit_entry entry = {q, &command, sizeof(command), 2};
   check(kbase_csf_submit(&entry, 1) == -1 && errno == EINVAL);
   check(kbase_csf_queue_published_target(q) == 0 && kick_calls == 0);
   entry.target = 1;
   check(kbase_csf_submit(&entry, 1) == 0);
   check(kbase_csf_queue_published_target(q) == 1);
   kbase_csf_queue_unref(q);
   finish_context(ctx, dev);
}

static void
test_seqno_error_and_retained_lifetime(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   check(q);
   kbase_csf_queue_ref(q);
   kbase_csf_context_unref(ctx);
   kbase_csf_queue_unref(q);
   check(close_calls == 0 && queue_term_calls == 0);
   uint64_t command = 0;
   check(kbase_csf_queue_prepare(q, &command, sizeof(command), 1) == 0);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == 0);
   *(uint64_t *)(uintptr_t)kbase_csf_queue_seqno_va(q) = 1;
   *(uint32_t *)(uintptr_t)(kbase_csf_queue_seqno_va(q) + 8) = 42;
   check(kbase_csf_wait((struct kbase_csf_queue *const[]){q}, 1,
                         (uint64_t[]){1}, 0) == -1 && errno == EIO);
   kbase_csf_queue_unref(q);
   check(close_calls == 1 && queue_term_calls == 1 && group_term_calls == 1);
   for (unsigned i = 0; i < mapping_count; i++)
      check(!mappings[i].live);
}

static void
test_sync_failure_and_wrap_padding(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   struct kbase_csf_queue *q = kbase_csf_queue_create(ctx, PAGE_SIZE, 1);
   check(q);
   uint8_t commands[128] = {0};
   check(kbase_csf_queue_prepare(q, commands, 8, 1) == 0);
   fail_sync = true;
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == -1);
   check(kick_calls == 0 && *(uint64_t *)((uint8_t *)user_io_base + PAGE_SIZE) == 0);
   fail_sync = false;
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == 0);
   *(uint64_t *)((uint8_t *)user_io_base + 2 * PAGE_SIZE) = 64;
   for (uint64_t target = 2; target <= 63; target++)
      check(kbase_csf_queue_prepare(q, commands, 8, target) == 0);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == 0);
   *(uint64_t *)((uint8_t *)user_io_base + 2 * PAGE_SIZE) = 4032;
   check(kbase_csf_queue_prepare(q, commands, sizeof(commands), 64) == 0);
   check(kbase_csf_publish((struct kbase_csf_queue *const[]){q}, 1) == 0);
   check(*(uint64_t *)((uint8_t *)user_io_base + PAGE_SIZE) == 4224);
   kbase_csf_queue_unref(q);
   finish_context(ctx, dev);
}

static void
test_kcpu_create_delete(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);

   struct kbase_csf_kcpu *kcpu = kbase_csf_kcpu_create(ctx);
   check(kcpu);
   check(kcpu_create_calls == 1 && kcpu_delete_calls == 0);

   kbase_csf_kcpu_destroy(kcpu);
   check(kcpu_delete_calls == 1);

   /* NULL is a no-op, not a crash. */
   kbase_csf_kcpu_destroy(NULL);

   finish_context(ctx, dev);
}

static void
test_kcpu_fence_signal(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   struct kbase_csf_kcpu *kcpu = kbase_csf_kcpu_create(ctx);
   check(kcpu);

   int fd = -1;
   check(kbase_csf_kcpu_fence_signal(kcpu, &fd) == 0);
   check(last_kcpu_type == BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL);
   check(kcpu_enqueue_calls == 1);

   /* The mock writes the descriptor back through base_fence, which is how the
    * kernel hands it to userspace. A negative fd here would mean the driver
    * never filled it in. */
   check(fd == 99);

   /* A failed enqueue must not report a descriptor. */
   fail_kcpu_enqueue = true;
   int bad_fd = -1;
   check(kbase_csf_kcpu_fence_signal(kcpu, &bad_fd) == -1 && errno == EINVAL);
   check(bad_fd == -1);
   fail_kcpu_enqueue = false;

   check(kbase_csf_kcpu_fence_signal(kcpu, NULL) == -1 && errno == EINVAL);
   check(kbase_csf_kcpu_fence_signal(NULL, &fd) == -1 && errno == EINVAL);

   /* The ioctl succeeded but the kernel never wrote a descriptor back. Reporting
    * success here would hand callers a bogus descriptor, so it must be EPROTO. */
   kcpu_fence_fd = -1;
   int unreported = -1;
   check(kbase_csf_kcpu_fence_signal(kcpu, &unreported) == -1 && errno == EPROTO);
   check(unreported == -1);
   kcpu_fence_fd = 99;

   kbase_csf_kcpu_destroy(kcpu);
   finish_context(ctx, dev);
}

static void
test_kcpu_cqs_wait(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);
   struct kbase_csf_kcpu *kcpu = kbase_csf_kcpu_create(ctx);
   check(kcpu);

   /* Address and threshold must reach the kernel unchanged: the kernel compares
    * *(uint32_t *)addr > val, so a swapped or truncated value silently waits on
    * the wrong condition. */
   check(kbase_csf_kcpu_cqs_wait(kcpu, 0x12345000ull, 7) == 0);
   check(last_kcpu_type == BASE_KCPU_COMMAND_TYPE_CQS_WAIT);
   check(last_cqs_addr == 0x12345000ull);
   check(last_cqs_val == 7);

   check(kbase_csf_kcpu_cqs_wait(NULL, 0x12345000ull, 7) == -1 && errno == EINVAL);

   kbase_csf_kcpu_destroy(kcpu);
   finish_context(ctx, dev);
}

static void
test_kcpu_create_failure(void)
{
   reset_fake();
   struct pan_kmod_dev *dev = NULL;
   struct kbase_csf_context *ctx = create_context(&dev);
   check(ctx);

   fail_kcpu_create = true;
   check(kbase_csf_kcpu_create(ctx) == NULL && errno == ENOMEM);
   fail_kcpu_create = false;

   check(kbase_csf_kcpu_create(NULL) == NULL && errno == EINVAL);

   finish_context(ctx, dev);
}

int main(void)
{
   test_sync_failure_and_wrap_padding();
   test_context_caps(); test_queue_failures(); test_legacy_group_zero_handle();
   test_prepare_publish_wait(); test_publish_failure_cancel_event();
   test_ring_backpressure_wrap_and_batch();
   test_atomic_submit_rollback();
   test_atomic_submit_cache_and_kick_failures();
   test_atomic_submit_stale_target();
   test_seqno_error_and_retained_lifetime();
   test_kcpu_create_failure();
   test_kcpu_create_delete();
   test_kcpu_fence_signal();
   test_kcpu_cqs_wait();
   return 0;
}
