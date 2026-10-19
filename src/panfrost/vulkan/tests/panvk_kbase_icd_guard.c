#define _GNU_SOURCE

#include "panvk_kbase_icd_guard.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#if !defined(__aarch64__) && !defined(__x86_64__)
#error "panvk kbase ICD guard supports only aarch64 and x86-64"
#endif

#define PANVK_MAX_RELOCS 1048576
#define PANVK_MAX_BINDINGS 128
#define PANVK_MAX_STRTAB (16 * 1024 * 1024)

struct image_info {
   uintptr_t base;
   uintptr_t start;
   uintptr_t end;
   const ElfW(Phdr) *phdr;
   ElfW(Half) phnum;
};

static bool
address_in_load_segments(const struct image_info *image, uintptr_t address,
                         size_t size)
{
   for (ElfW(Half) i = 0; i < image->phnum; i++) {
      const ElfW(Phdr) *ph = &image->phdr[i];
      if (ph->p_type != PT_LOAD)
         continue;
      uintptr_t start = image->base + ph->p_vaddr;
      if (address >= start && address - start <= ph->p_memsz &&
          size <= ph->p_memsz - (address - start))
         return true;
   }
   return false;
}

static int
find_image_callback(struct dl_phdr_info *info, size_t size, void *data)
{
   struct image_info *image = data;
   if ((uintptr_t)info->dlpi_addr != image->base)
      return 0;

   image->phdr = info->dlpi_phdr;
   image->phnum = info->dlpi_phnum;
   for (ElfW(Half) i = 0; i < image->phnum; i++) {
      const ElfW(Phdr) *ph = &image->phdr[i];
      if (ph->p_type != PT_LOAD)
         continue;
      uintptr_t start = image->base + ph->p_vaddr;
      uintptr_t end = start + ph->p_memsz;
      if (end < start)
         return 1;
      if (image->start == 0 || start < image->start)
         image->start = start;
      if (end > image->end)
         image->end = end;
   }
   return 1;
}

static bool
symbol_image(void *reference, void *symbol)
{
   Dl_info ref_info = {0};
   Dl_info symbol_info = {0};
   if (reference == NULL || symbol == NULL ||
       dladdr(reference, &ref_info) == 0 ||
       dladdr(symbol, &symbol_info) == 0 ||
       ref_info.dli_fbase == NULL || symbol_info.dli_fbase == NULL)
      return false;
   return ref_info.dli_fbase == symbol_info.dli_fbase;
}

bool
panvk_icd_symbol_in_image(void *reference, void *symbol)
{
   return symbol_image(reference, symbol);
}

struct dynamic_info {
   ElfW(Sym) *symtab;
   const char *strtab;
   size_t strsz;
   ElfW(Rela) *jmprel;
   size_t pltrelsz;
   ElfW(Rela) *rela;
   size_t relasz;
   bool plt_is_rela;
};

static bool
resolve_dynamic_pointer(const struct image_info *image, ElfW(Addr) value,
                        size_t size, void **result)
{
   uintptr_t pointer = (uintptr_t)value;
   if (address_in_load_segments(image, pointer, size)) {
      *result = (void *)pointer;
      return true;
   }
   if (pointer > image->end - image->base)
      return false;
   pointer += image->base;
   if (!address_in_load_segments(image, pointer, size))
      return false;
   *result = (void *)pointer;
   return true;
}

static bool
read_dynamic(struct image_info *image, struct dynamic_info *dynamic)
{
   ElfW(Dyn) *entries = NULL;
   size_t entry_count = 0;
   for (ElfW(Half) i = 0; i < image->phnum; i++) {
      const ElfW(Phdr) *ph = &image->phdr[i];
      if (ph->p_type == PT_DYNAMIC) {
         uintptr_t address = image->base + ph->p_vaddr;
         if (!address_in_load_segments(image, address, ph->p_memsz) ||
             ph->p_memsz < sizeof(ElfW(Dyn)))
            return false;
         entries = (ElfW(Dyn) *)address;
         entry_count = ph->p_memsz / sizeof(*entries);
         break;
      }
   }
   if (entries == NULL)
      return false;

   ElfW(Addr) symtab = 0, strtab = 0, jmprel = 0, rela = 0;
   size_t strsz = 0, pltrelsz = 0, relasz = 0;
   ElfW(Sword) pltrel = 0;
   bool found_strsz = false, found_pltrel = false;
   for (size_t i = 0; i < entry_count; i++) {
      if (entries[i].d_tag == DT_NULL)
         break;
      switch (entries[i].d_tag) {
      case DT_SYMTAB: symtab = entries[i].d_un.d_ptr; break;
      case DT_STRTAB: strtab = entries[i].d_un.d_ptr; break;
      case DT_STRSZ: strsz = entries[i].d_un.d_val; found_strsz = true; break;
      case DT_JMPREL: jmprel = entries[i].d_un.d_ptr; break;
      case DT_PLTRELSZ: pltrelsz = entries[i].d_un.d_val; break;
      case DT_PLTREL: pltrel = entries[i].d_un.d_val; found_pltrel = true; break;
      case DT_RELA: rela = entries[i].d_un.d_ptr; break;
      case DT_RELASZ: relasz = entries[i].d_un.d_val; break;
      case DT_HASH:
      case DT_GNU_HASH:
         break;
      default: break;
      }
   }
   if (symtab == 0 || strtab == 0 || !found_strsz || strsz == 0 ||
       strsz > PANVK_MAX_STRTAB)
      return false;
   memset(dynamic, 0, sizeof(*dynamic));
   void *ptr = NULL;
   if (!resolve_dynamic_pointer(image, symtab, sizeof(ElfW(Sym)), &ptr))
      return false;
   dynamic->symtab = ptr;
   if (!resolve_dynamic_pointer(image, strtab, strsz, &ptr))
      return false;
   dynamic->strtab = ptr;
   dynamic->strsz = strsz;
   if (pltrelsz != 0) {
      if (!found_pltrel || pltrel != DT_RELA ||
          pltrelsz % sizeof(ElfW(Rela)) != 0 ||
          pltrelsz / sizeof(ElfW(Rela)) > PANVK_MAX_RELOCS ||
          !resolve_dynamic_pointer(image, jmprel, pltrelsz, &ptr))
         return false;
      dynamic->jmprel = ptr;
      dynamic->pltrelsz = pltrelsz;
      dynamic->plt_is_rela = true;
   }
   if (relasz != 0) {
      if (relasz % sizeof(ElfW(Rela)) != 0 ||
          relasz / sizeof(ElfW(Rela)) > PANVK_MAX_RELOCS ||
          !resolve_dynamic_pointer(image, rela, relasz, &ptr))
         return false;
      dynamic->rela = ptr;
      dynamic->relasz = relasz;
   }
   return true;
}

static bool
check_relocations(struct image_info *image, const struct dynamic_info *dynamic,
                  const struct panvk_icd_binding *bindings, size_t count,
                  bool *seen)
{
   size_t tables = 0;
   ElfW(Rela) *table[2] = { dynamic->jmprel, dynamic->rela };
   size_t bytes[2] = { dynamic->pltrelsz, dynamic->relasz };
   for (size_t t = 0; t < 2; t++) {
      if (table[t] == NULL || bytes[t] == 0)
         continue;
      if (t == 1 && table[0] == table[1])
         continue;
      size_t n = bytes[t] / sizeof(ElfW(Rela));
      for (size_t i = 0; i < n; i++) {
         ElfW(Rela) *rel = &table[t][i];
#if defined(__aarch64__)
         unsigned type = ELF64_R_TYPE(rel->r_info);
         unsigned sym_index = ELF64_R_SYM(rel->r_info);
         bool relevant = type == R_AARCH64_JUMP_SLOT || type == R_AARCH64_GLOB_DAT;
#elif defined(__x86_64__)
         unsigned type = ELF64_R_TYPE(rel->r_info);
         unsigned sym_index = ELF64_R_SYM(rel->r_info);
         bool relevant = type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT;
#endif
         if (!relevant)
            continue;
         if (sym_index > PANVK_MAX_RELOCS ||
             !address_in_load_segments(image,
                (uintptr_t)dynamic->symtab + sym_index * sizeof(ElfW(Sym)),
                sizeof(ElfW(Sym))))
            return false;
         ElfW(Sym) *sym = &dynamic->symtab[sym_index];
         if (sym->st_name >= dynamic->strsz)
            return false;
         const char *name = dynamic->strtab + sym->st_name;
         size_t remaining = dynamic->strsz - sym->st_name;
         if (memchr(name, '\0', remaining) == NULL)
            return false;
         size_t binding = count;
         for (size_t b = 0; b < count; b++) {
            if (strcmp(name, bindings[b].name) == 0) {
               binding = b;
               break;
            }
         }
         if (binding == count)
            continue;
         uintptr_t slot_address = image->base + rel->r_offset;
         if (!address_in_load_segments(image, slot_address, sizeof(uintptr_t)))
            return false;
         void *slot_value = *(void **)slot_address;
         if (slot_value != bindings[binding].address) {
            fprintf(stderr, "panvk ICD guard: %s relocation mismatch\n", name);
            return false;
         }
         seen[binding] = true;
         tables++;
      }
   }
   return tables != 0;
}

bool
panvk_icd_verify_bindings(void *icd_symbol,
                          const struct panvk_icd_binding *bindings,
                          size_t count)
{
   if (!icd_symbol || !bindings || !count || count > PANVK_MAX_BINDINGS) {
      fprintf(stderr, "panvk ICD guard: invalid binding arguments\n");
      return false;
   }
   for (size_t i = 0; i < count; i++)
      if (!bindings[i].name || !bindings[i].address)
         return false;
   struct image_info image = {0};
   Dl_info info = {0};
   if (dladdr(icd_symbol, &info) == 0 || info.dli_fbase == NULL) {
      fprintf(stderr, "panvk ICD guard: cannot identify ICD image\n");
      return false;
   }
   image.base = (uintptr_t)info.dli_fbase;
   dl_iterate_phdr(find_image_callback, &image);
   if (image.phdr == NULL || image.start == 0 || image.end <= image.start) {
      fprintf(stderr, "panvk ICD guard: ICD program headers unavailable\n");
      return false;
   }
   struct dynamic_info dynamic;
   if (!read_dynamic(&image, &dynamic)) {
      fprintf(stderr, "panvk ICD guard: unsupported or invalid dynamic tables\n");
      return false;
   }
   bool seen[PANVK_MAX_BINDINGS] = {0};
   if (!check_relocations(&image, &dynamic, bindings, count, seen)) {
      fprintf(stderr, "panvk ICD guard: no verified matching relocations\n");
      return false;
   }
   for (size_t i = 0; i < count; i++) {
      if (bindings[i].name == NULL || bindings[i].address == NULL || !seen[i]) {
         fprintf(stderr, "panvk ICD guard: missing binding %s\n",
                 bindings[i].name != NULL ? bindings[i].name : "(null)");
         return false;
      }
   }
   return true;
}

bool
panvk_icd_install_syscall_guard(void)
{
   struct sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               (uint32_t)offsetof(struct seccomp_data, arch)),
#if defined(__aarch64__)
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
#elif defined(__x86_64__)
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
#endif
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               (uint32_t)offsetof(struct seccomp_data, nr)),
#if defined(__x86_64__)
      BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x40000000, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#endif
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#ifdef SYS_open
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_open, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef SYS_openat2
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_openat2, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_ioctl, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
   };
   struct sock_fprog program = {
      .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
      .filter = filter,
   };
   if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
       prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
      fprintf(stderr, "panvk ICD guard: seccomp installation failed: %s\n",
              strerror(errno));
      return false;
   }
   return true;
}
