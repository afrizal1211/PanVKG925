# Copyright © 2023 Collabora
# SPDX-License-Identifier: MIT

libpankmod_lib_args = [no_override_init_args]
libpankmod_lib_link_with = []
libpankmod_lib_inc = []
libpankmod_lib_code = [
  'pan_kmod.c',
]

if panfrost_kmds.contains('panfrost') or with_panfrost_vdrm or with_gallium_panfrost
  libpankmod_lib_code += 'panfrost_kmod.c'
endif
if panfrost_kmds.contains('panthor') or with_gallium_panfrost or with_panfrost_vk
  libpankmod_lib_code += 'panthor_kmod.c'
endif
if panfrost_kmds.contains('kbase')
  libpankmod_lib_code += ['kbase_kmod.c', 'kbase_sync.c', 'kbase_csf.c']
endif

if with_panfrost_vdrm
  libpankmod_lib_inc += [inc_virtio_gpu, inc_virtio_vdrm, include_directories('virtio')]
  libpankmod_lib_args += '-DHAVE_PANFROST_VDRM'
  libpankmod_lib_link_with += libvdrm
endif

libpankmod_lib_files = files(libpankmod_lib_code)

libpankmod_lib = static_library(
  'pankmod_lib',
  [libpankmod_lib_files],
  include_directories : [inc_include, inc_src, inc_panfrost, libpankmod_lib_inc],
  c_args : libpankmod_lib_args,
  gnu_symbol_visibility : 'hidden',
  dependencies: [dep_libdrm, idep_mesautil, idep_pan_packers, libpanfrost_model_dep],
  build_by_default : false,
  link_with: libpankmod_lib_link_with
)

libpankmod_dep = declare_dependency(
  include_directories: [inc_include, libpankmod_lib_inc],
  dependencies: [dep_libdrm],
)

if with_tests and panfrost_kmds.contains('kbase') and host_machine.system() == 'linux'
  kbase_kmod_test = executable(
    'kbase_kmod_test',
    files('tests/kbase_kmod_test.c', '../pan_props.c', '../pan_trace.c'),
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('.')],
    dependencies: [dep_libdrm, idep_mesautil, idep_pan_packers, libpanfrost_model_dep],
    link_with: libpankmod_lib,
    link_args: [
      '-Wl,--wrap=ioctl',
      '-Wl,--wrap=open', '-Wl,--wrap=open64',
      '-Wl,--wrap=mmap',
      '-Wl,--wrap=mmap64',
      '-Wl,--wrap=munmap',
      '-Wl,--wrap=close',
      '-Wl,--wrap=drmGetVersion',
    ],
  )
  test('pan_kmod_kbase_mock', kbase_kmod_test, suite: ['pan-kmod-host'])
  test('pan_kmod_kbase_default_probe', kbase_kmod_test,
       args: ['--default-probe'], suite: ['pan-kmod-host'])
  test('pan_kmod_kbase_explicit_probe', kbase_kmod_test,
       args: ['--explicit-probe'], suite: ['pan-kmod-host'])
  kbase_csf_test = executable(
    'kbase_csf_test',
    files('tests/kbase_csf_test.c', '../pan_props.c', '../pan_trace.c'),
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('.')],
    dependencies: [dep_libdrm, idep_mesautil, idep_pan_packers, libpanfrost_model_dep],
    link_with: libpankmod_lib,
    link_args: [
      '-Wl,--wrap=ioctl', '-Wl,--wrap=mmap', '-Wl,--wrap=mmap64',
      '-Wl,--wrap=munmap', '-Wl,--wrap=close', '-Wl,--wrap=drmGetVersion',
      '-Wl,--wrap=ppoll', '-Wl,--wrap=read',
    ],
  )
  test('pan_kmod_kbase_csf', kbase_csf_test,
       suite: ['pan-kmod-host'], timeout: 30)
endif
