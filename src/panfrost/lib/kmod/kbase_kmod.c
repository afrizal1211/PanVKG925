/*
 * Copyright © 2026 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-heap.h>

#include "drm-uapi/mali_kbase_ioctl.h"
#include "kbase_kmod.h"
#include "kbase_sync.h"
#include "pan_kmod_backend.h"
#include "pan_model.h"
#include "util/u_atomic.h"

#define KBASE_PAGE_SIZE 4096ull
#define KBASE_MAX_PROPS_SIZE (1024 * 1024)
#define KBASE_VA_RESERVE 0x2000000ull

struct kbase_kmod_dev {
   struct pan_kmod_dev base;
   void *tracking_page;
   uint32_t next_handle;
   simple_mtx_t jit_lock;
   uint64_t jit_va_pages;
   uint64_t jit_phys_pages;
   uint32_t jit_max_allocations;
   bool jit_initialized;
   bool is_csf;
   bool alias_cleanup_uncertain;
   int dma_heap_fd;
};

struct kbase_kmod_heap {
   struct kbase_kmod_dev *dev;
   uint64_t context;
   uint64_t first_chunk;
};

struct kbase_kmod_alias {
   struct pan_kmod_bo *bo;
   uint64_t gpu_va;
   size_t size;
   void *mapping;
};

struct kbase_kmod_bo {
   struct pan_kmod_bo base;
   void *cpu_ptr;
   uint64_t gpu_va;
   uint64_t memory_flags;
   bool same_va;
   /* dma-buf fd backing this BO, or -1.  Both dma-heap-allocated BOs and
    * externally imported BOs keep a dup here so bo_export() can hand out a
    * fresh fd without involving the kernel. */
   int dmabuf_fd;
   /* CPU mapping of dmabuf_fd, or NULL.  Kept separate from cpu_ptr: the
    * latter is the kbase placement mapping (PROT_NONE for imported cookies)
    * and cannot be used for CPU access. */
   void *dmabuf_cpu;
};

static uint64_t
kbase_read_le(const uint8_t *data, unsigned size)
{
   uint64_t value = 0;
   for (unsigned i = 0; i < size; i++)
      value |= (uint64_t)data[i] << (i * 8);
   return value;
}

static uint64_t
kbase_gpuprop_get(const uint8_t *buf, size_t size, uint32_t key)
{
   size_t offset = 0;
   while (offset < size) {
      uint32_t header = kbase_read_le(buf + offset, 4);
      unsigned value_size = 1u << (header & 3);
      offset += 4;
      if ((header >> 2) == key)
         return kbase_read_le(buf + offset, value_size);
      offset += value_size;
   }
   return 0;
}

bool
kbase_kmod_parse_props(const uint8_t *buf, size_t size,
                       struct pan_kmod_dev_props *props)
{
   if (!buf || !props || !size || size > KBASE_MAX_PROPS_SIZE) {
      errno = EINVAL;
      return false;
   }

   size_t offset = 0;
   while (offset < size) {
      if (size - offset < 4) {
         errno = EINVAL;
         return false;
      }
      uint32_t header = kbase_read_le(buf + offset, 4);
      unsigned value_size = 1u << (header & 3);
      offset += 4;
      if (size - offset < value_size) {
         errno = EINVAL;
         return false;
      }
      offset += value_size;
   }

   struct pan_kmod_dev_props result = {0};
   result.gpu_id = kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_GPU_ID);
   if (!result.gpu_id) {
      uint64_t product = kbase_gpuprop_get(buf, size, KBASE_GPUPROP_PRODUCT_ID);
      uint64_t major = kbase_gpuprop_get(buf, size, KBASE_GPUPROP_MAJOR_REVISION);
      uint64_t minor = kbase_gpuprop_get(buf, size, KBASE_GPUPROP_MINOR_REVISION);
      uint64_t status = kbase_gpuprop_get(buf, size, KBASE_GPUPROP_VERSION_STATUS);
      if (product > UINT16_MAX || major > 15 || minor > 255 || status > 15) {
         errno = EINVAL;
         return false;
      }
      result.gpu_id = (product << 16) | (major << 12) | (minor << 4) | status;
   }

   result.gpu_variant =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_CORE_FEATURES) & 0xff;
   result.gpu_features =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_GPU_FEATURES);
   result.shader_present =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_SHADER_PRESENT);
   result.tiler_features =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_TILER_FEATURES);
   result.mem_features =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_MEM_FEATURES);
   result.mmu_features =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_MMU_FEATURES);
   result.l2_features =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_L2_FEATURES);
   for (unsigned i = 0; i < 3; i++)
      result.texture_features[i] =
         kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0 + i);
   result.texture_features[3] =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3);
   result.max_threads_per_core =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_THREAD_MAX_THREADS);
   result.max_threads_per_wg =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE);
   uint32_t thread_features =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_RAW_THREAD_FEATURES);
   result.max_tasks_per_core = MAX2(thread_features >> 24, 1);
   result.num_registers_per_core = thread_features & 0xffff;
   result.max_tls_instance_per_core =
      kbase_gpuprop_get(buf, size, KBASE_GPUPROP_TLS_ALLOC);
   if (!result.max_tls_instance_per_core)
      result.max_tls_instance_per_core = result.max_threads_per_core;
   result.pgsize_bitmap = PAN_PGSIZE_4K;
   result.supported_bo_flags = PAN_KMOD_BO_FLAG_EXECUTABLE |
                               PAN_KMOD_BO_FLAG_NO_MMAP |
                               PAN_KMOD_BO_FLAG_WB_MMAP |
                               PAN_KMOD_BO_FLAG_GPU_UNCACHED |
                               PAN_KMOD_BO_FLAG_CSF_EVENT;

   unsigned va_bits = MMU_FEATURES_VA_BITS(result.mmu_features);
   if (!result.gpu_id || !result.shader_present || va_bits < 32 || va_bits > 48) {
      errno = EINVAL;
      return false;
   }
   *props = result;
   return true;
}

static uint8_t *
kbase_get_gpuprops(int fd, size_t *out_size)
{
   struct kbase_ioctl_get_gpuprops req = {0};
   int size = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req);
   if (size <= 0 || size > KBASE_MAX_PROPS_SIZE) {
      if (size >= 0)
         errno = EINVAL;
      return NULL;
   }

   uint8_t *buf = calloc(1, size);
   if (!buf)
      return NULL;
   req.buffer = (uintptr_t)buf;
   req.size = size;
   int ret = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &req);
   if (ret < 0 || ret > size) {
      int error = ret < 0 ? errno : EINVAL;
      free(buf);
      errno = error;
      return NULL;
   }
   *out_size = ret ? ret : size;
   return buf;
}

static struct pan_kmod_dev *
kbase_kmod_dev_create(int fd, uint32_t flags,
                       const struct pan_kmod_allocator *allocator)
{
   if (!allocator || (flags & ~(PAN_KMOD_DEV_FLAG_OWNS_FD |
                               PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL))) {
      errno = EINVAL;
      return NULL;
   }

   struct kbase_ioctl_version_check ver = {0};
   bool is_csf = ioctl(fd, KBASE_IOCTL_VERSION_CHECK_CSF, &ver) == 0;
   if (!is_csf) {
      ver = (struct kbase_ioctl_version_check){0};
      if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK_JM, &ver))
         return NULL;
   }
   if ((is_csf && ver.major != 1) || (!is_csf && ver.major < 11)) {
      errno = ENOTSUP;
      return NULL;
   }

   struct kbase_ioctl_set_flags set_flags = {0};
   if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags))
      return NULL;
   void *tracking = mmap(NULL, KBASE_PAGE_SIZE, PROT_NONE, MAP_SHARED, fd,
                          BASE_MEM_MAP_TRACKING_HANDLE);
   if (tracking == MAP_FAILED)
      return NULL;

   size_t props_size = 0;
   uint8_t *props_buf = kbase_get_gpuprops(fd, &props_size);
   struct pan_kmod_dev_props props;
   if (!props_buf || !kbase_kmod_parse_props(props_buf, props_size, &props)) {
      int error = errno;
      free(props_buf);
      munmap(tracking, KBASE_PAGE_SIZE);
      errno = error;
      return NULL;
   }
   free(props_buf);

   struct kbase_kmod_dev *dev = pan_kmod_alloc(allocator, sizeof(*dev));
   if (!dev) {
      int error = ENOMEM;
      munmap(tracking, KBASE_PAGE_SIZE);
      errno = error;
      return NULL;
   }
   dev->dma_heap_fd = -1;
   struct util_sync_provider *sync = kbase_sync_provider_create();
   if (!sync) {
      int error = errno;
      pan_kmod_free(allocator, dev);
      munmap(tracking, KBASE_PAGE_SIZE);
      errno = error;
      return NULL;
   }

   struct pan_kmod_driver driver = {
      .version = {.major = ver.major, .minor = ver.minor},
   };
   if (!is_csf || !pan_kmod_driver_version_at_least(&driver, 1, 9)) {
      struct kbase_ioctl_mem_exec_init exec_init = {.va_pages = 0x100000};
      if (ioctl(fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec_init))
         props.supported_bo_flags &= ~PAN_KMOD_BO_FLAG_EXECUTABLE;
   }

   flags |= PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL;
   if (pan_kmod_dev_init(&dev->base, fd, flags, &driver, allocator,
                         &kbase_kmod_ops, sync)) {
      sync->finalize(sync);
      pan_kmod_free(allocator, dev);
      munmap(tracking, KBASE_PAGE_SIZE);
      errno = ENOMEM;
      return NULL;
   }
   dev->base.props = props;
   if (!is_csf)
      dev->base.props.supported_bo_flags &= ~PAN_KMOD_BO_FLAG_CSF_EVENT;
   dev->tracking_page = tracking;
   simple_mtx_init(&dev->jit_lock, mtx_plain);
   dev->is_csf = is_csf;

   /* Opt-in dma-buf backing for WSI/export.  Without this, BOs are allocated
    * through MEM_ALLOC and cannot be exported (kbase has no dma-buf exporter).
    * Keeping it behind an environment variable preserves the default path. */
   const char *heap_path = getenv("PANVK_KBASE_DMA_HEAP");
   if (heap_path && heap_path[0] && strcmp(heap_path, "0") != 0) {
      dev->dma_heap_fd = open(heap_path, O_RDONLY | O_CLOEXEC);
      if (dev->dma_heap_fd < 0)
         fprintf(stderr, "kbase: dma-heap %s unavailable: %s\n", heap_path,
                 strerror(errno));
   }
   return &dev->base;
}

static void
kbase_kmod_dev_destroy(struct pan_kmod_dev *base)
{
   struct kbase_kmod_dev *dev = container_of(base, struct kbase_kmod_dev, base);
   const struct pan_kmod_allocator *allocator = base->allocator;
   if (dev->dma_heap_fd >= 0)
      close(dev->dma_heap_fd);
   munmap(dev->tracking_page, KBASE_PAGE_SIZE);
   pan_kmod_dev_cleanup(base);
   simple_mtx_destroy(&dev->jit_lock);
   pan_kmod_free(allocator, dev);
}

static struct pan_kmod_va_range
kbase_kmod_dev_query_user_va_range(const struct pan_kmod_dev *dev)
{
   unsigned bits = MMU_FEATURES_VA_BITS(dev->props.mmu_features);
   return (struct pan_kmod_va_range){
      .start = KBASE_VA_RESERVE,
      .size = (1ull << (bits - 1)) - KBASE_VA_RESERVE,
   };
}

static uint64_t
to_kbase_mem_flags(uint32_t flags)
{
   uint64_t value = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                    BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                    BASE_MEM_COHERENT_LOCAL;
   if (flags & PAN_KMOD_BO_FLAG_EXECUTABLE) {
      value |= BASE_MEM_PROT_GPU_EX;
      value &= ~BASE_MEM_PROT_GPU_WR;
   } else {
      value |= BASE_MEM_SAME_VA;
   }
   if (flags & PAN_KMOD_BO_FLAG_WB_MMAP)
      value |= BASE_MEM_CACHED_CPU;
   if (flags & PAN_KMOD_BO_FLAG_GPU_UNCACHED)
      value |= BASE_MEM_UNCACHED_GPU;
   if (flags & PAN_KMOD_BO_FLAG_CSF_EVENT) {
      value |= BASE_MEM_CSF_EVENT;
      value &= ~BASE_MEM_COHERENT_LOCAL;
   }
   return value;
}

static struct pan_kmod_bo *kbase_kmod_bo_import(struct pan_kmod_dev *base,
                                                 int fd);

/* Allocate a BO from the dma-heap and import it into kbase.  The import keeps
 * a dup of the dma-buf fd, so bo_export() can hand the same buffer to a
 * compositor/WSI without a kernel-side dma-buf exporter. */
static struct pan_kmod_bo *
kbase_kmod_bo_alloc_dmabuf(struct pan_kmod_dev *base, uint64_t size)
{
   struct kbase_kmod_dev *dev = container_of(base, struct kbase_kmod_dev, base);
   struct dma_heap_allocation_data alloc = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };

   if (ioctl(dev->dma_heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc)) {
      fprintf(stderr, "kbase: DMA_HEAP_IOCTL_ALLOC(%llu) failed: %s\n",
              (unsigned long long)size, strerror(errno));
      return NULL;
   }

   struct pan_kmod_bo *bo = kbase_kmod_bo_import(base, alloc.fd);
   close(alloc.fd);
   return bo;
}

static struct pan_kmod_bo *
kbase_kmod_bo_alloc(struct pan_kmod_dev *base, struct pan_kmod_vm *vm,
                     uint64_t size, uint32_t flags)
{
   if (!size || size > SIZE_MAX - (KBASE_PAGE_SIZE - 1) ||
       (flags & ~base->props.supported_bo_flags) ||
       ((flags & PAN_KMOD_BO_FLAG_CSF_EVENT) &&
        (flags & (PAN_KMOD_BO_FLAG_EXECUTABLE | PAN_KMOD_BO_FLAG_NO_MMAP |
                  PAN_KMOD_BO_FLAG_GPU_UNCACHED))) ||
       (vm && vm->dev != base)) {
      errno = EINVAL;
      return NULL;
   }
   size = ALIGN_POT(size, KBASE_PAGE_SIZE);
   struct kbase_kmod_dev *dev = container_of(base, struct kbase_kmod_dev, base);
   if (!vm && dev->dma_heap_fd >= 0 &&
       !(flags & (PAN_KMOD_BO_FLAG_EXECUTABLE |
                  PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT |
                  PAN_KMOD_BO_FLAG_CSF_EVENT)))
      return kbase_kmod_bo_alloc_dmabuf(base, size);
   uint32_t previous;
   do {
      previous = p_atomic_read(&dev->next_handle);
      if (previous == UINT32_MAX) {
         errno = ENOSPC;
         return NULL;
      }
   } while (p_atomic_cmpxchg(&dev->next_handle, previous, previous + 1) != previous);

   struct kbase_kmod_bo *bo = pan_kmod_dev_alloc(base, sizeof(*bo));
   if (!bo) {
      errno = ENOMEM;
      return NULL;
   }
   bo->dmabuf_fd = -1;
   bo->dmabuf_cpu = NULL;
   uint64_t memory_flags = to_kbase_mem_flags(flags);
   uint64_t handle;
   if (dev->is_csf && pan_kmod_driver_version_at_least(&base->driver, 1, 9)) {
      union kbase_ioctl_mem_alloc_ex req = {
         .in = {
            .va_pages = size / KBASE_PAGE_SIZE,
            .commit_pages = size / KBASE_PAGE_SIZE,
            .flags = memory_flags,
         },
      };
      if (ioctl(base->fd, KBASE_IOCTL_MEM_ALLOC_EX, &req))
         goto error;
      memory_flags = req.out.flags;
      handle = req.out.gpu_va;
   } else {
      union kbase_ioctl_mem_alloc req = {
         .in = {
            .va_pages = size / KBASE_PAGE_SIZE,
            .commit_pages = size / KBASE_PAGE_SIZE,
            .flags = memory_flags,
         },
      };
      if (ioctl(base->fd, KBASE_IOCTL_MEM_ALLOC, &req))
         goto error;
      memory_flags = req.out.flags;
      handle = req.out.gpu_va;
   }

   bo->same_va = (memory_flags & BASE_MEM_SAME_VA) != 0;
   bo->memory_flags = memory_flags;
   if (!handle || handle > INT64_MAX || (handle & (KBASE_PAGE_SIZE - 1)) ||
       bo->same_va == !!(flags & PAN_KMOD_BO_FLAG_EXECUTABLE)) {
      errno = EINVAL;
      goto error_free_memory;
   }
   int prot = (flags & PAN_KMOD_BO_FLAG_NO_MMAP) ? PROT_NONE :
                                                PROT_READ | PROT_WRITE;
   bo->cpu_ptr = mmap(NULL, size, prot, MAP_SHARED, base->fd, handle);
   if (bo->cpu_ptr == MAP_FAILED)
      goto error_free_memory;
   bo->gpu_va = bo->same_va ? (uintptr_t)bo->cpu_ptr : handle;
   pan_kmod_bo_init(&bo->base, base, vm, size, flags, previous + 1);
   return &bo->base;

error_free_memory: {
   int error = errno;
   struct kbase_ioctl_mem_free req = {.gpu_addr = handle};
   ioctl(base->fd, KBASE_IOCTL_MEM_FREE, &req);
   errno = error;
}
error: {
   int error = errno;
   pan_kmod_dev_free(base, bo);
   errno = error;
   return NULL;
}
}

static void
kbase_kmod_bo_free(struct pan_kmod_bo *base)
{
   struct kbase_kmod_bo *bo = container_of(base, struct kbase_kmod_bo, base);
   pan_kmod_bo_cleanup(base);
   if (bo->dmabuf_cpu)
      munmap(bo->dmabuf_cpu, base->size);
   if (bo->cpu_ptr)
      munmap(bo->cpu_ptr, base->size);
   if (!bo->same_va) {
      struct kbase_ioctl_mem_free req = {.gpu_addr = bo->gpu_va};
      ioctl(base->dev->fd, KBASE_IOCTL_MEM_FREE, &req);
   }
   if (bo->dmabuf_fd >= 0)
      close(bo->dmabuf_fd);
   pan_kmod_dev_free(base->dev, bo);
}

static struct pan_kmod_bo *
kbase_kmod_bo_import(struct pan_kmod_dev *base, int fd)
{
   if (!base->ops || base->ops != &kbase_kmod_ops || fd < 0) {
      errno = EINVAL;
      return NULL;
   }

   struct kbase_kmod_dev *dev = container_of(base, struct kbase_kmod_dev, base);
   uint32_t previous;
   do {
      previous = p_atomic_read(&dev->next_handle);
      if (previous == UINT32_MAX) {
         errno = ENOSPC;
         return NULL;
      }
   } while (p_atomic_cmpxchg(&dev->next_handle, previous, previous + 1) != previous);

   struct kbase_kmod_bo *bo = pan_kmod_dev_alloc(base, sizeof(*bo));
   if (!bo) {
      errno = ENOMEM;
      return NULL;
   }
   bo->dmabuf_fd = -1;
   bo->dmabuf_cpu = NULL;

   int handle_fd = fd;
   /* Stage 1: import through UMM and request explicit cache maintenance at
    * map/unmap time.  The pan_kmod_bo_import() primitive has no flags
    * argument, so the kbase backend carries this flag itself.  UNCACHED_GPU
    * stays out of the default path; it is a stage-3 experiment.
    */
   const uint64_t import_flags =
      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
      BASE_MEM_IMPORT_SYNC_ON_MAP_UNMAP;
   union kbase_ioctl_mem_import req = {
      .in = {
         .flags = import_flags,
         .phandle = (uintptr_t)&handle_fd,
         .type = BASE_MEM_IMPORT_TYPE_UMM,
      },
   };
   if (ioctl(base->fd, KBASE_IOCTL_MEM_IMPORT, &req))
      goto error;

   uint64_t cookie = req.out.gpu_va;
   uint64_t pages = req.out.va_pages;
   bool valid = cookie && cookie <= INT64_MAX &&
                !(cookie & (KBASE_PAGE_SIZE - 1)) &&
                pages && pages <= SIZE_MAX / KBASE_PAGE_SIZE &&
                !(req.out.flags & (BASE_MEM_UNCACHED_GPU | BASE_MEM_PROT_GPU_EX));
   if (!valid) {
      /* The output is unverified, so we cannot tell whether the cookie belongs
       * to this call.  Only local cleanup is safe.
       */
      errno = EINVAL;
      goto error;
   }

   uint64_t size = pages * KBASE_PAGE_SIZE;
   /* kbase_mem_import() has two shapes:
    *
    *  - NEED_MMAP/SAME_VA set: the returned value is a cookie, and the region
    *    only enters GPU VA space once it is mmap()ed.  For a 64-bit client
    *    this is the only shape that occurs, because kbase_mem_from_umm()
    *    forces NEED_MMAP.
    *  - otherwise the kernel reserved and mapped the region itself and the
    *    returned value is already a real GPU VA.
    */
   if (req.out.flags & (BASE_MEM_NEED_MMAP | BASE_MEM_SAME_VA)) {
      /* PROT_NONE is required, not merely preferable.  The mmap consistency
       * check is "CPU_RD || !VM_READ" and "CPU_WR || !VM_WRITE", so a mapping
       * with neither VM_READ nor VM_WRITE always passes, while asking for CPU
       * access would demand CPU_RD/CPU_WR import flags that are themselves
       * pointless here: the fault handler refuses CPU faults on imported UMM
       * mapped through a cookie ("Invalid CPU access to UMM memory").
       *
       * The mapping exists only to make the kernel place the region; it is
       * never handed to callers (PAN_KMOD_BO_FLAG_NO_MMAP below).  CPU access
       * has to go through the dma-buf fd.
       */
      void *mapping = mmap(NULL, size, PROT_NONE, MAP_SHARED, base->fd, cookie);
      if (mapping == MAP_FAILED)
         goto error_free_import;
      uintptr_t start = (uintptr_t)mapping;
      if (!start || start > UINTPTR_MAX - (size - 1) ||
          (start >> 32) != ((start + size - 1) >> 32)) {
         if (munmap(mapping, size)) {
            dev->alias_cleanup_uncertain = true;
            errno = EIO;
            goto error;
         }
         errno = EOVERFLOW;
         goto error;
      }
      bo->cpu_ptr = mapping;
      bo->gpu_va = start;
      bo->same_va = true;
   } else {
      /* Real GPU VA, no CPU mapping at all, and the region is not torn down by
       * munmap, so bo_free has to issue MEM_FREE.
       */
      bo->cpu_ptr = NULL;
      bo->gpu_va = cookie;
      bo->same_va = false;
   }

   bo->dmabuf_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (bo->dmabuf_fd >= 0) {
      /* Keep a CPU mapping of the exporter's buffer.  External O_RDONLY
       * imports fail here and simply keep NO_MMAP; dma-heap BOs have an
       * O_RDWR fd and succeed. */
      void *cpu = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       bo->dmabuf_fd, 0);
      if (cpu != MAP_FAILED)
         bo->dmabuf_cpu = cpu;
   }
   bo->memory_flags = req.out.flags;
   pan_kmod_bo_init(&bo->base, base, NULL, size,
                    PAN_KMOD_BO_FLAG_IMPORTED | PAN_KMOD_BO_FLAG_NO_MMAP,
                    previous + 1);
   if (bo->dmabuf_cpu)
      bo->base.flags &= ~PAN_KMOD_BO_FLAG_NO_MMAP;
   return &bo->base;

error_free_import:
   /* Only reachable from the cookie branch: the region exists but was never
    * mapped, so MEM_FREE is the right way to release it.
    */
   {
      int error = errno;
      struct kbase_ioctl_mem_free free_req = {.gpu_addr = cookie};
      ioctl(base->fd, KBASE_IOCTL_MEM_FREE, &free_req);
      errno = error;
   }
error: {
   int error = errno;
   pan_kmod_dev_free(base, bo);
   errno = error;
   return NULL;
   }
}

static int
kbase_kmod_bo_export(struct pan_kmod_bo *base)
{
   struct kbase_kmod_bo *bo = container_of(base, struct kbase_kmod_bo, base);

   if (bo->dmabuf_fd < 0) {
      errno = ENOTSUP;
      return -1;
   }

   return fcntl(bo->dmabuf_fd, F_DUPFD_CLOEXEC, 3);
}

static void *
kbase_kmod_bo_mmap(struct pan_kmod_bo *base, int prot, int flags,
                    void *host_addr)
{
   struct kbase_kmod_bo *bo = container_of(base, struct kbase_kmod_bo, base);
   if (bo->dmabuf_cpu) {
      if ((host_addr && host_addr != bo->dmabuf_cpu) || flags != MAP_SHARED ||
          prot != (PROT_READ | PROT_WRITE)) {
         errno = ENOTSUP;
         return MAP_FAILED;
      }
      return bo->dmabuf_cpu;
   }
   if ((base->flags & PAN_KMOD_BO_FLAG_NO_MMAP) ||
       (host_addr && host_addr != bo->cpu_ptr) || flags != MAP_SHARED ||
       prot != (PROT_READ | PROT_WRITE)) {
      errno = ENOTSUP;
      return MAP_FAILED;
   }
   return bo->cpu_ptr;
}

static int
kbase_kmod_bo_munmap(struct pan_kmod_bo *base, void *host_addr, size_t size)
{
   struct kbase_kmod_bo *bo = container_of(base, struct kbase_kmod_bo, base);
   if (bo->dmabuf_cpu && host_addr == bo->dmabuf_cpu && size <= base->size)
      return 0;
   uintptr_t address = (uintptr_t)host_addr;
   uintptr_t start = (uintptr_t)bo->cpu_ptr;
   if (!size || address < start || address - start >= base->size ||
       size > base->size - (address - start)) {
      errno = EINVAL;
      return -1;
   }
   return 0;
}

/* kbase has no per-BO completion object. Waiting on a BO in the DRM backends
 * means "is this buffer's GPU access finished", which they answer with a
 * reservation object. kbase tracks completion per CSF queue as a seqno cell
 * and has nothing BO-scoped to query, so this cannot be implemented without
 * inventing a mapping from BOs to whatever job happened to touch them.
 *
 * Unreachable today: the only caller is panfrost_bo_wait() in Gallium, and
 * panfrost_open_device() refuses kbase outright.
 */
static bool
kbase_kmod_bo_wait(UNUSED struct pan_kmod_bo *bo, UNUSED int64_t timeout_ns,
                    UNUSED bool for_read_only_access)
{
   errno = ENOTSUP;
   return false;
}

bool
kbase_kmod_is_csf(const struct pan_kmod_dev *dev)
{
   if (!dev || dev->ops != &kbase_kmod_ops)
      return false;
   const struct kbase_kmod_dev *kdev =
      container_of(dev, const struct kbase_kmod_dev, base);
   return kdev->is_csf;
}

bool
kbase_kmod_supports_panvk(const struct pan_kmod_dev *dev)
{
   return kbase_kmod_is_csf(dev) &&
          dev->driver.version.major == 1 && dev->driver.version.minor == 30 &&
          pan_prod_id(dev->props.gpu_id) == PAN_PROD_ID(13, 8, 0) &&
          dev->props.gpu_variant == 4 &&
          util_bitcount64(dev->props.shader_present) == 12 &&
          (dev->props.gpu_features & (1ull << 2)) &&
          MMU_FEATURES_VA_BITS(dev->props.mmu_features) == 48;
}

struct pan_kmod_dev *
kbase_kmod_dev_reopen(const struct pan_kmod_dev *physical, uint32_t flags,
                      const struct pan_kmod_allocator *allocator)
{
   if (!physical || physical->ops != &kbase_kmod_ops || !allocator ||
       (flags & ~(PAN_KMOD_DEV_FLAG_OWNS_FD |
                  PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL))) {
      errno = EINVAL;
      return NULL;
   }

   char path[64];
   int length = snprintf(path, sizeof(path), "/proc/self/fd/%d", physical->fd);
   if (length < 0 || (size_t)length >= sizeof(path)) {
      errno = EINVAL;
      return NULL;
   }

   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return NULL;

   flags |= physical->flags;
   flags &= ~PAN_KMOD_DEV_FLAG_OWNS_FD;
   flags |= PAN_KMOD_DEV_FLAG_OWNS_FD;
   struct pan_kmod_dev *dev = kbase_kmod_dev_create(fd, flags, allocator);
   if (!dev) {
      int error = errno;
      close(fd);
      errno = error;
   }
   return dev;
}

uint64_t
kbase_kmod_bo_gpu_va(struct pan_kmod_bo *bo)
{
   if (!bo || bo->dev->ops != &kbase_kmod_ops)
      return 0;
   return container_of(bo, struct kbase_kmod_bo, base)->gpu_va;
}

/* Cache maintenance on a BO.
 *
 * NO_MMAP normally means "no CPU mapping to touch", so syncing is pointless.
 * Imported dma-buf BOs are the exception: they are NO_MMAP because the kernel
 * refuses CPU faults on them, yet they still own a CPU mapping that the
 * exporter can maintain.  The kernel's MEM_SYNC takes the user address and
 * calls dma_buf_begin_cpu_access()/end_cpu_access() with it; it never
 * dereferences it.  Treat the imported case as syncable so callers can
 * invalidate the CPU cache before reading data the GPU wrote.
 */
int
kbase_kmod_bo_sync(struct pan_kmod_bo *bo, uint64_t offset, uint64_t size,
                    enum pan_kmod_bo_sync_type type)
{
   if (!bo || bo->dev->ops != &kbase_kmod_ops || !size ||
       offset > bo->size || size > bo->size - offset ||
       type != PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH &&
       type != PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE) {
      errno = EINVAL;
      return -1;
   }
   struct kbase_kmod_bo *kbo = container_of(bo, struct kbase_kmod_bo, base);
   /* A NO_MMAP BO that is not imported has no mapping at all. */
   if ((bo->flags & PAN_KMOD_BO_FLAG_NO_MMAP) &&
       !(bo->flags & PAN_KMOD_BO_FLAG_IMPORTED)) {
      errno = EINVAL;
      return -1;
   }
   if (!kbo->cpu_ptr) {
      errno = EINVAL;
      return -1;
   }
   struct kbase_ioctl_mem_sync req = {
      .handle = kbo->gpu_va,
      .user_addr = (uintptr_t)kbo->cpu_ptr + offset,
      .size = size,
      .type = type == PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH ?
                 BASE_SYNCSET_OP_MSYNC : BASE_SYNCSET_OP_CSYNC,
   };
   return ioctl(bo->dev->fd, KBASE_IOCTL_MEM_SYNC, &req);
}

static int
kbase_kmod_flush_bo_map_syncs(struct pan_kmod_dev *dev)
{
   util_dynarray_foreach(&dev->pending_bo_syncs.array,
                         struct pan_kmod_deferred_bo_sync, sync) {
      struct kbase_kmod_bo *bo =
         container_of(sync->bo, struct kbase_kmod_bo, base);
      if (sync->start > sync->bo->size || sync->size > sync->bo->size - sync->start) {
         errno = EINVAL;
         return -1;
      }
      struct kbase_ioctl_mem_sync req = {
         .handle = bo->gpu_va,
         .user_addr = (uintptr_t)bo->cpu_ptr + sync->start,
         .size = sync->size,
         .type = sync->type == PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH ?
                    BASE_SYNCSET_OP_MSYNC : BASE_SYNCSET_OP_CSYNC,
      };
      if (ioctl(dev->fd, KBASE_IOCTL_MEM_SYNC, &req))
         return -1;
   }
   return 0;
}

int
kbase_kmod_jit_init(struct pan_kmod_dev *base, uint64_t va_pages,
                    uint64_t phys_pages, uint32_t max_allocations)
{
   if (!base || base->ops != &kbase_kmod_ops || !va_pages || !phys_pages ||
       max_allocations < 1 || max_allocations > UINT8_MAX) {
      errno = EINVAL;
      return -1;
   }
   unsigned bits = MMU_FEATURES_VA_BITS(base->props.mmu_features);
   if (va_pages > ((1ull << bits) / KBASE_PAGE_SIZE) || phys_pages > va_pages) {
      errno = EINVAL;
      return -1;
   }

   struct kbase_kmod_dev *dev = container_of(base, struct kbase_kmod_dev, base);
   simple_mtx_lock(&dev->jit_lock);
   if (dev->jit_initialized) {
      bool identical = dev->jit_va_pages == va_pages &&
                       dev->jit_phys_pages == phys_pages &&
                       dev->jit_max_allocations == max_allocations;
      simple_mtx_unlock(&dev->jit_lock);
      if (!identical)
         errno = EINVAL;
      return identical ? 0 : -1;
   }

   struct kbase_ioctl_mem_jit_init req = {
      .va_pages = va_pages,
      .max_allocations = max_allocations,
      .group_id = 0,
      .trim_level = 0,
      .phys_pages = phys_pages,
   };
   if (ioctl(base->fd, KBASE_IOCTL_MEM_JIT_INIT, &req)) {
      simple_mtx_unlock(&dev->jit_lock);
      return -1;
   }
   dev->jit_va_pages = va_pages;
   dev->jit_phys_pages = phys_pages;
   dev->jit_max_allocations = max_allocations;
   dev->jit_initialized = true;
   simple_mtx_unlock(&dev->jit_lock);
   return 0;
}

struct kbase_kmod_heap *
kbase_kmod_heap_create(struct pan_kmod_dev *base, uint32_t chunk_size,
                       uint32_t initial_chunks, uint32_t max_chunks,
                       uint32_t target_in_flight, uint32_t memory_group)
{
   if (!base || base->ops != &kbase_kmod_ops || chunk_size < KBASE_PAGE_SIZE ||
       (chunk_size & (KBASE_PAGE_SIZE - 1)) || !initial_chunks || !max_chunks ||
       initial_chunks > max_chunks || !target_in_flight || target_in_flight > UINT16_MAX ||
       memory_group > UINT8_MAX || (uint64_t)chunk_size * max_chunks > UINT32_MAX) {
      errno = EINVAL;
      return NULL;
   }
   struct kbase_kmod_dev *dev = container_of(base, struct kbase_kmod_dev, base);
   if (!dev->jit_initialized) {
      errno = EINVAL;
      return NULL;
   }

   struct kbase_kmod_heap *heap = pan_kmod_dev_alloc(base, sizeof(*heap));
   if (!heap) {
      errno = ENOMEM;
      return NULL;
   }
   union kbase_ioctl_cs_tiler_heap_init req = {
      .in = {
         .chunk_size = chunk_size,
         .initial_chunks = initial_chunks,
         .max_chunks = max_chunks,
         .target_in_flight = target_in_flight,
         .group_id = memory_group,
      },
   };
   if (ioctl(base->fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &req)) {
      int error = errno;
      pan_kmod_dev_free(base, heap);
      errno = error;
      return NULL;
   }
   uint64_t context = req.out.gpu_heap_va;
   uint64_t first_chunk = req.out.first_chunk_va;
   /* The heap context VA is not page aligned and must not be required to be.
    * kbase hands out a slot inside a shared context pool, spaced by
    * HEAP_CTX_SIZE (32) rounded up to the GPU cache line, so the second and
    * later heaps in a context land on non-page offsets. Only the chunk
    * address, which is a real mapping, has to be page aligned.
    */
   if (!context || !first_chunk || (first_chunk & (KBASE_PAGE_SIZE - 1)) ||
       first_chunk > UINT64_MAX - chunk_size) {
      int error = EINVAL;
      struct kbase_ioctl_cs_tiler_heap_term term_req = {.gpu_heap_va = context};
      ioctl(base->fd, KBASE_IOCTL_CS_TILER_HEAP_TERM, &term_req);
      pan_kmod_dev_free(base, heap);
      errno = error;
      return NULL;
   }

   heap->dev = dev;
   heap->context = context;
   heap->first_chunk = first_chunk;
   return heap;
}

int
kbase_kmod_heap_destroy(struct kbase_kmod_heap *heap)
{
   if (!heap) {
      errno = EINVAL;
      return -1;
   }
   struct kbase_ioctl_cs_tiler_heap_term req = {.gpu_heap_va = heap->context};
   if (ioctl(heap->dev->base.fd, KBASE_IOCTL_CS_TILER_HEAP_TERM, &req))
      return -1;
   pan_kmod_dev_free(&heap->dev->base, heap);
   return 0;
}

uint64_t
kbase_kmod_heap_context(struct kbase_kmod_heap *heap)
{
   return heap ? heap->context : 0;
}

uint64_t
kbase_kmod_heap_first_chunk(struct kbase_kmod_heap *heap)
{
   return heap ? heap->first_chunk : 0;
}

struct kbase_kmod_alias *
kbase_kmod_alias_create(struct pan_kmod_bo *base, uint64_t size, uint32_t count)
{
   if (!base || base->dev->ops != &kbase_kmod_ops || !size ||
       (size & (KBASE_PAGE_SIZE - 1)) || !count || count > 4 ||
       size > base->size || size > (1ull << 32) / count || size > SIZE_MAX / count ||
       (base->flags & (PAN_KMOD_BO_FLAG_GPU_UNCACHED | PAN_KMOD_BO_FLAG_EXECUTABLE |
                       PAN_KMOD_BO_FLAG_CSF_EVENT | PAN_KMOD_BO_FLAG_NO_MMAP))) {
      errno = EINVAL;
      return NULL;
   }
   struct kbase_kmod_bo *bo = container_of(base, struct kbase_kmod_bo, base);
   if (!(bo->memory_flags & BASE_MEM_SAME_VA)) {
      errno = EINVAL;
      return NULL;
   }
   struct kbase_kmod_alias *alias = pan_kmod_dev_alloc(base->dev, sizeof(*alias));
   if (!alias) {
      errno = ENOMEM;
      return NULL;
   }
   struct base_mem_aliasing_info entries[4] = {0};
   for (uint32_t i = 0; i < count; i++) {
      entries[i].handle = bo->gpu_va;
      entries[i].offset = 0;
      entries[i].length = size / KBASE_PAGE_SIZE;
   }
   size_t total = size * count;
   for (unsigned attempt = 0; attempt < 16; attempt++) {
      union kbase_ioctl_mem_alias req = {
         .in = {
            .flags = BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                     BASE_MEM_PROT_CPU_RD,
            .stride = size / KBASE_PAGE_SIZE,
            .nents = count,
            .aliasing_info = (uintptr_t)entries,
         },
      };
      if (ioctl(base->dev->fd, KBASE_IOCTL_MEM_ALIAS, &req))
         goto fail;
      uint64_t cookie = req.out.gpu_va;
      bool valid = cookie && cookie <= INT64_MAX &&
                   !(cookie & (KBASE_PAGE_SIZE - 1)) &&
                   req.out.va_pages == total / KBASE_PAGE_SIZE &&
                   (req.out.flags & BASE_MEM_NEED_MMAP) &&
                   !(req.out.flags & (BASE_MEM_UNCACHED_GPU | BASE_MEM_PROT_GPU_EX));
      if (!valid) {
         struct kbase_kmod_dev *kdev = container_of(base->dev, struct kbase_kmod_dev, base);
         kdev->alias_cleanup_uncertain = true;
         errno = EINVAL;
         goto fail;
      }
      void *mapping = mmap(NULL, total, PROT_READ, MAP_SHARED, base->dev->fd, cookie);
      if (mapping == MAP_FAILED) {
         int error = errno;
         struct kbase_ioctl_mem_free free_req = {.gpu_addr = cookie};
         ioctl(base->dev->fd, KBASE_IOCTL_MEM_FREE, &free_req);
         errno = error;
         goto fail;
      }
      uintptr_t start = (uintptr_t)mapping;
      if (start && start <= UINTPTR_MAX - (total - 1) &&
          (start >> 32) == ((start + total - 1) >> 32)) {
         alias->bo = pan_kmod_bo_get(base);
         alias->gpu_va = start;
         alias->size = total;
         alias->mapping = mapping;
         return alias;
      }
      if (munmap(mapping, total)) {
         struct kbase_kmod_dev *kdev = container_of(base->dev, struct kbase_kmod_dev, base);
         kdev->alias_cleanup_uncertain = true;
         errno = EIO;
         goto fail;
      }
   }
   errno = ENOSPC;
fail: {
      int error = errno;
      pan_kmod_dev_free(base->dev, alias);
      errno = error;
      return NULL;
   }
}

int
kbase_kmod_alias_destroy(struct kbase_kmod_alias *alias)
{
   if (!alias) {
      errno = EINVAL;
      return -1;
   }
   if (munmap(alias->mapping, alias->size))
      return -1;
   struct pan_kmod_dev *dev = alias->bo->dev;
   pan_kmod_bo_put(alias->bo);
   pan_kmod_dev_free(dev, alias);
   return 0;
}

uint64_t
kbase_kmod_alias_gpu_va(struct kbase_kmod_alias *alias)
{
   return alias ? alias->gpu_va : 0;
}

bool
kbase_kmod_alias_cleanup_uncertain(const struct pan_kmod_dev *dev)
{
   if (!dev || dev->ops != &kbase_kmod_ops)
      return false;
   const struct kbase_kmod_dev *kdev =
      container_of(dev, const struct kbase_kmod_dev, base);
   return kdev->alias_cleanup_uncertain;
}

static struct pan_kmod_vm *
kbase_kmod_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                      uint64_t va_start, uint64_t va_range)
{
   if (flags != PAN_KMOD_VM_FLAG_AUTO_VA || va_start || va_range) {
      errno = ENOTSUP;
      return NULL;
   }
   struct pan_kmod_vm *vm = pan_kmod_dev_alloc(dev, sizeof(*vm));
   if (!vm) {
      errno = ENOMEM;
      return NULL;
   }
   pan_kmod_vm_init(vm, dev, 0, flags);
   return vm;
}

static void
kbase_kmod_vm_destroy(struct pan_kmod_vm *vm)
{
   pan_kmod_vm_cleanup(vm);
   pan_kmod_dev_free(vm->dev, vm);
}

static int
kbase_kmod_vm_bind(struct pan_kmod_vm *vm, enum pan_kmod_vm_op_mode mode,
                    struct pan_kmod_vm_op *ops, uint32_t count)
{
   if (mode != PAN_KMOD_VM_OP_MODE_IMMEDIATE) {
      errno = ENOTSUP;
      return -1;
   }
   if (count && !ops) {
      errno = EINVAL;
      return -1;
   }
   for (unsigned i = 0; i < count; i++) {
      struct pan_kmod_vm_op *op = &ops[i];
      if (pan_kmod_vm_op_check(vm, mode, op) || op->signal.count ||
          op->wait.count || op->flags) {
         errno = ENOTSUP;
         return -1;
      }
      if (!op->va.size || (op->va.size & (KBASE_PAGE_SIZE - 1))) {
         errno = EINVAL;
         return -1;
      }
      if (op->type == PAN_KMOD_VM_OP_TYPE_MAP) {
         struct pan_kmod_bo *bo = op->map.bo;
         if (!bo || bo->dev != vm->dev ||
             (bo->exclusive_vm && bo->exclusive_vm != vm) ||
             op->map.bo_offset < 0 ||
             ((uint64_t)op->map.bo_offset & (KBASE_PAGE_SIZE - 1)) ||
             (uint64_t)op->map.bo_offset > bo->size ||
             op->va.size > bo->size - op->map.bo_offset) {
            errno = EINVAL;
            return -1;
         }
      } else if (op->type == PAN_KMOD_VM_OP_TYPE_UNMAP) {
         unsigned bits = MMU_FEATURES_VA_BITS(vm->dev->props.mmu_features);
         if (!op->va.start || (op->va.start & (KBASE_PAGE_SIZE - 1)) ||
             op->va.start >= (1ull << bits) ||
             op->va.size > (1ull << bits) - op->va.start) {
            errno = EINVAL;
            return -1;
         }
      } else {
         errno = ENOTSUP;
         return -1;
      }
   }
   for (unsigned i = 0; i < count; i++) {
      if (ops[i].type == PAN_KMOD_VM_OP_TYPE_MAP) {
         struct kbase_kmod_bo *bo =
            container_of(ops[i].map.bo, struct kbase_kmod_bo, base);
         ops[i].va.start = bo->gpu_va + ops[i].map.bo_offset;
      }
   }
   return 0;
}

static uint64_t
kbase_kmod_query_timestamp(UNUSED const struct pan_kmod_dev *dev)
{
   errno = ENOTSUP;
   return 0;
}

static struct pan_kmod_perf_session *
kbase_kmod_perf_create(UNUSED struct pan_kmod_dev *dev)
{
   errno = ENOTSUP;
   return NULL;
}

const struct pan_kmod_ops kbase_kmod_ops = {
   .dev_create = kbase_kmod_dev_create,
   .dev_destroy = kbase_kmod_dev_destroy,
   .dev_query_user_va_range = kbase_kmod_dev_query_user_va_range,
   .bo_alloc = kbase_kmod_bo_alloc,
   .bo_free = kbase_kmod_bo_free,
   .bo_import = kbase_kmod_bo_import,
   .bo_export = kbase_kmod_bo_export,
   .bo_mmap = kbase_kmod_bo_mmap,
   .bo_munmap = kbase_kmod_bo_munmap,
   .flush_bo_map_syncs = kbase_kmod_flush_bo_map_syncs,
   .bo_wait = kbase_kmod_bo_wait,
   .vm_create = kbase_kmod_vm_create,
   .vm_destroy = kbase_kmod_vm_destroy,
   .vm_bind = kbase_kmod_vm_bind,
   .query_timestamp = kbase_kmod_query_timestamp,
   .perf_create = kbase_kmod_perf_create,
};
