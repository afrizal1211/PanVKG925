#include "kbase_sync.h"

#include "util/macros.h"
#include "util/u_sync_provider.h"

#include <errno.h>
#include <stdlib.h>

static int
kbase_sync_unsupported_create(UNUSED struct util_sync_provider *p, UNUSED uint32_t flags, uint32_t *handle)
{
   if (handle != NULL)
      *handle = 0;
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_destroy(UNUSED struct util_sync_provider *p, UNUSED uint32_t handle)
{
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_handle_to_fd(UNUSED struct util_sync_provider *p, UNUSED uint32_t handle, int *out_obj_fd)
{
   if (out_obj_fd != NULL)
      *out_obj_fd = -1;
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_fd_to_handle(UNUSED struct util_sync_provider *p, UNUSED int obj_fd, uint32_t *handle)
{
   if (handle != NULL)
      *handle = 0;
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_import_sync_file(UNUSED struct util_sync_provider *p, UNUSED uint32_t handle, UNUSED int sync_file_fd)
{
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_export_sync_file(UNUSED struct util_sync_provider *p, UNUSED uint32_t handle, int *out_sync_file_fd)
{
   if (out_sync_file_fd != NULL)
      *out_sync_file_fd = -1;
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_wait(UNUSED struct util_sync_provider *p, UNUSED uint32_t *handles, UNUSED unsigned num_handles,
                            UNUSED int64_t timeout_nsec, UNUSED unsigned flags, uint32_t *first_signaled)
{
   if (first_signaled != NULL)
      *first_signaled = 0;
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_reset(UNUSED struct util_sync_provider *p, UNUSED const uint32_t *handles, UNUSED uint32_t handle_count)
{
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_signal(UNUSED struct util_sync_provider *p, UNUSED const uint32_t *handles, UNUSED uint32_t handle_count)
{
   errno = ENOTSUP;
   return -1;
}

static int
kbase_sync_unsupported_transfer(UNUSED struct util_sync_provider *p, UNUSED uint32_t dst_handle, UNUSED uint64_t dst_point,
                                UNUSED uint32_t src_handle, UNUSED uint64_t src_point, UNUSED uint32_t flags)
{
   errno = ENOTSUP;
   return -1;
}

static void
kbase_sync_finalize(struct util_sync_provider *p)
{
   free(p);
}

static struct util_sync_provider *
kbase_sync_clone(UNUSED struct util_sync_provider *p)
{
   return kbase_sync_provider_create();
}

struct util_sync_provider *
kbase_sync_provider_create(void)
{
   struct util_sync_provider *p = calloc(1, sizeof(*p));
   if (p == NULL)
      return NULL;

   *p = (struct util_sync_provider) {
      .create = kbase_sync_unsupported_create,
      .destroy = kbase_sync_unsupported_destroy,
      .handle_to_fd = kbase_sync_unsupported_handle_to_fd,
      .fd_to_handle = kbase_sync_unsupported_fd_to_handle,
      .import_sync_file = kbase_sync_unsupported_import_sync_file,
      .export_sync_file = kbase_sync_unsupported_export_sync_file,
      .wait = kbase_sync_unsupported_wait,
      .reset = kbase_sync_unsupported_reset,
      .signal = kbase_sync_unsupported_signal,
      .timeline_signal = NULL,
      .timeline_wait = NULL,
      .query = NULL,
      .transfer = kbase_sync_unsupported_transfer,
      .finalize = kbase_sync_finalize,
      .clone = kbase_sync_clone,
   };

   return p;
}
