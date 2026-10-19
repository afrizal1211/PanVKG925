#ifndef PANVK_KBASE_ICD_GUARD_H
#define PANVK_KBASE_ICD_GUARD_H

#include <stdbool.h>
#include <stddef.h>

struct panvk_icd_binding {
   const char *name;
   void *address;
};

bool panvk_icd_verify_bindings(void *icd_symbol,
                               const struct panvk_icd_binding *bindings,
                               size_t count);
bool panvk_icd_symbol_in_image(void *reference, void *symbol);
bool panvk_icd_install_syscall_guard(void);

#endif
