# Copyright © 2021 Collabora Ltd.
#
# Derived from the freedreno driver which is:
# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

panvk_entrypoints = custom_target(
  'panvk_entrypoints.[ch]',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['panvk_entrypoints.h', 'panvk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'panvk',
    '--device-prefix', 'panvk_v6', '--device-prefix', 'panvk_v7',
    '--device-prefix', 'panvk_v9', '--device-prefix', 'panvk_v10',
    '--device-prefix', 'panvk_v11', '--device-prefix', 'panvk_v12',
    '--device-prefix', 'panvk_v13', '--device-prefix', 'panvk_v14',
    '--beta', with_vulkan_beta.to_string()
  ],
  depend_files : vk_entrypoints_gen_depend_files,
)

panvk_drirc = custom_target(
  'panvk_drirc',
  input  : [files('panvk_drirc_gen.py'), drirc_gen, '00-panvk-defaults.conf'],
  output : ['panvk_drirc.c', 'panvk_drirc.h'],
  command : [
    prog_python, '@INPUT0@',
    '--import-path', join_paths(dir_source_root, 'src/util'),
    '--drirc-src', '@OUTPUT0@',
    '--drirc-hdr', '@OUTPUT1@',
    '--validate', '@INPUT2@',
  ],
)

panvk_tracepoints = custom_target(
  'panvk_tracepoints.[ch]',
  input: 'panvk_tracepoints.py',
  output: ['panvk_tracepoints.h',
           'panvk_tracepoints_perfetto.h',
           'panvk_tracepoints.c'],
  command: [
    prog_python, '@INPUT@',
    '--import-path', join_paths(dir_source_root, 'src/util/perf/'),
    '--utrace-hdr', '@OUTPUT0@',
    '--perfetto-hdr', '@OUTPUT1@',
    '--utrace-src', '@OUTPUT2@',
  ],
  depend_files: u_trace_py,
)

libpanvk_files = files(
  'panvk_async_bind.c',
  'panvk_bind_queue.c',
  'panvk_buffer.c',
  'panvk_cmd_pool.c',
  'panvk_device_memory.c',
  'panvk_host_copy.c',
  'panvk_image.c',
  'panvk_instance.c',
  'panvk_mempool.c',
  'panvk_nir_lower_cooperative_matrix.c',
  'panvk_nir_lower_tile_image.c',
  'panvk_physical_device.c',
  'panvk_priv_bo.c',
  'panvk_utrace.c',
  'panvk_wsi.c',
)
libpanvk_files += [sha1_h]

if panfrost_kmds.contains('kbase')
  libpanvk_files += files('panvk_kbase_sync.c', 'panvk_kbase_csf_sync.c',
                         'panvk_kbase_csf_submit.c', 'panvk_kbase_device.c',
                         'panvk_kbase_semaphore.c')
endif

panvk_deps = []
panvk_flags = []
panvk_per_arch_libs = []

bifrost_archs = [6, 7]
bifrost_inc_dir = ['bifrost']
bifrost_files = [
  'bifrost/panvk_vX_meta_desc_copy.c',
]

valhall_archs = [9, 10, 11]
valhall_inc_dir = ['valhall']
valhall_files = []

fifthgen_archs = [12, 13, 14]
fifthgen_inc_dir = ['fifthgen']
fifthgen_files = []

jm_archs = [6, 7]
jm_inc_dir = ['jm']
jm_files = [
  'jm/panvk_vX_cmd_buffer.c',
  'jm/panvk_vX_cmd_dispatch.c',
  'jm/panvk_vX_cmd_draw.c',
  'jm/panvk_vX_cmd_event.c',
  'jm/panvk_vX_cmd_query.c',
  'jm/panvk_vX_cmd_precomp.c',
  'jm/panvk_vX_event.c',
  'jm/panvk_vX_gpu_queue.c',
]

csf_archs = [10, 11, 12, 13, 14]
csf_inc_dir = ['csf']
csf_files = [
  'csf/panvk_vX_cmd_buffer.c',
  'csf/panvk_vX_cmd_cond_render.c',
  'csf/panvk_vX_cmd_dispatch.c',
  'csf/panvk_vX_cmd_draw.c',
  'csf/panvk_vX_cmd_event.c',
  'csf/panvk_vX_cmd_query.c',
  'csf/panvk_vX_cmd_precomp.c',
  'csf/panvk_vX_event.c',
  'csf/panvk_vX_exception_handler.c',
  'csf/panvk_vX_gpu_queue.c',
  'csf/panvk_vX_instr.c',
  'csf/panvk_vX_utrace.c',
]

common_per_arch_files = [
  panvk_entrypoints[0],
  panvk_tracepoints[0],
  panvk_drirc[1],
  'panvk_vX_blend.c',
  'panvk_vX_buffer_view.c',
  'panvk_vX_cmd_desc_state.c',
  'panvk_vX_cmd_dispatch.c',
  'panvk_vX_cmd_draw.c',
  'panvk_vX_cmd_frame_shaders.c',
  'panvk_vX_cmd_meta.c',
  'panvk_vX_cmd_push_constant.c',
  'panvk_vX_descriptor_set.c',
  'panvk_vX_descriptor_set_layout.c',
  'panvk_vX_device.c',
  'panvk_vX_physical_device.c',
  'panvk_vX_precomp_cache.c',
  'panvk_vX_query_pool.c',
  'panvk_vX_image_view.c',
  'panvk_vX_nir_lower_descriptors.c',
  'panvk_vX_nir_lower_input_attachment_loads.c',
  'panvk_vX_sampler.c',
  'panvk_vX_shader.c',
  sha1_h,
]

foreach arch : [6, 7, 10, 11, 12, 13, 14]
  per_arch_files = common_per_arch_files
  inc_panvk_per_arch = []

  if arch in bifrost_archs
    inc_panvk_per_arch += bifrost_inc_dir
    per_arch_files += bifrost_files
  elif arch in valhall_archs
    inc_panvk_per_arch += valhall_inc_dir
    per_arch_files += valhall_files
  elif arch in fifthgen_archs
    inc_panvk_per_arch += fifthgen_inc_dir
    per_arch_files += fifthgen_files
  endif

  if arch in jm_archs
    inc_panvk_per_arch += jm_inc_dir
    per_arch_files += jm_files
  elif arch in csf_archs
    inc_panvk_per_arch += csf_inc_dir
    per_arch_files += csf_files
    if panfrost_kmds.contains('kbase')
      per_arch_files += files('panvk_vX_kbase_csf_encode.c')
    endif
  endif

  panvk_per_arch_libs += static_library(
    'panvk_v@0@'.format(arch),
    per_arch_files,
    include_directories : [
      inc_include,
      inc_src,
      inc_panfrost,
      inc_panvk_per_arch,
    ],
    dependencies : [
      idep_nir_headers,
      idep_pan_packers,
      idep_vulkan_util_headers,
      idep_vulkan_runtime_headers,
      idep_vulkan_wsi_headers,
      idep_mesautil,
      dep_libdrm,
      dep_valgrind,
      idep_libpan_per_arch[arch.to_string()],
    ],
    c_args : [no_override_init_args, panvk_flags, soong_compat_c_args, '-DPAN_ARCH=@0@'.format(arch)],
    gnu_symbol_visibility : 'hidden',
  )
endforeach

if with_perfetto
  panvk_deps += dep_perfetto
  libpanvk_files += ['panvk_utrace_perfetto.cc']
endif

if with_platform_wayland
  panvk_deps += dep_wayland_client
endif

if with_platform_android
  libpanvk_files += files('panvk_android.c')
endif

libvulkan_panfrost = shared_library(
  'vulkan_panfrost',
  [libpanvk_files, panvk_entrypoints, panvk_tracepoints, panvk_drirc],
  include_directories : [
    inc_include,
    inc_src,
    inc_panfrost,
  ],
  link_whole : [panvk_per_arch_libs],
  link_with : [
    libpanfrost_shared,
    libpanfrost_decode,
    libpanfrost_lib,
    libpanfrost_compiler,
  ],
  dependencies : [
    dep_dl,
    dep_elf,
    dep_libdrm,
    dep_m,
    dep_thread,
    dep_valgrind,
    idep_nir,
    idep_pan_packers,
    panvk_deps,
    idep_vulkan_util,
    idep_vulkan_runtime,
    idep_vulkan_wsi,
    idep_mesautil,
  ],
  c_args : [no_override_init_args, panvk_flags, soong_compat_c_args],
  link_args : [vulkan_icd_link_args, ld_args_bsymbolic, ld_args_gc_sections, ld_args_build_id],
  link_depends: vulkan_icd_link_depends,
  gnu_symbol_visibility : 'hidden',
  install : true,
)

# TODO: Re-enable the symbol check with Kraid
if with_symbols_check
  test(
    'panvk symbols check',
    symbols_check,
    args : [
      '--lib', libvulkan_panfrost,
      '--symbols-file', vulkan_icd_symbols,
      symbols_check_args,
    ],
    suite : ['panfrost'],
  )
endif

icd_file_name = libname_prefix + 'vulkan_panfrost.' + libname_suffix

panfrost_icd = custom_target(
  'panfrost_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'panfrost_icd.' + vulkan_manifest_suffix,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', vulkan_icd_lib_path,
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install_tag : 'runtime',
  install : true,
)

_dev_icdname = 'panfrost_devenv_icd.@0@.json'.format(host_machine.cpu())
_dev_icd = custom_target(
  'panfrost_devenv_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : _dev_icdname,
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.4', '--xml', '@INPUT1@',
    '--sizeof-pointer', sizeof_pointer,
    '--icd-lib-path', meson.current_build_dir(),
    '--icd-filename', icd_file_name,
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
)

devenv.append('VK_DRIVER_FILES', _dev_icd.full_path())

if with_tests and panfrost_kmds.contains('kbase')
  panvk_kbase_sync_test = executable(
    'panvk_kbase_sync_test',
    files('tests/panvk_kbase_sync_test.c', 'panvk_kbase_sync.c'),
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('.')],
    dependencies: [idep_vulkan_runtime, idep_mesautil, dep_thread],
    c_args: [no_override_init_args],
  )
  test('panvk_kbase_sync_host', panvk_kbase_sync_test,
       suite: ['pan-kmod-host'], timeout: 30)
  panvk_kbase_csf_sync_test = executable(
    'panvk_kbase_csf_sync_test',
    files('tests/panvk_kbase_csf_sync_test.c', 'panvk_kbase_sync.c',
          'panvk_kbase_csf_sync.c'),
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('.')],
    dependencies: [idep_vulkan_runtime, idep_mesautil, dep_thread],
    c_args: [no_override_init_args, '-UNDEBUG'],
    link_args: [
      '-Wl,--wrap=kbase_csf_queue_ref', '-Wl,--wrap=kbase_csf_queue_unref',
      '-Wl,--wrap=kbase_csf_queue_published_target', '-Wl,--wrap=kbase_csf_wait',
    ],
  )
  test('panvk_kbase_csf_sync', panvk_kbase_csf_sync_test,
       suite: ['pan-kmod-host'], timeout: 30)
  foreach arch : csf_archs
    kbase_encode_test = executable(
      'panvk_kbase_csf_encode_test_v@0@'.format(arch),
      files('tests/panvk_kbase_csf_encode_test.c', 'panvk_vX_kbase_csf_encode.c'),
      include_directories: [inc_include, inc_src, inc_panfrost, include_directories('.')],
      dependencies: [idep_pan_packers, idep_mesautil],
      c_args: [no_override_init_args, '-UNDEBUG', '-DPAN_ARCH=@0@'.format(arch)],
    )
    test('panvk_kbase_csf_encode_v@0@'.format(arch), kbase_encode_test,
         suite: ['pan-kmod-host'], timeout: 30)
  endforeach
  kbase_submit_test = executable(
    'panvk_kbase_csf_submit_test',
    files('tests/panvk_kbase_csf_submit_test.c', 'panvk_kbase_sync.c',
          'panvk_kbase_csf_sync.c', 'panvk_kbase_csf_submit.c'),
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('.')],
    dependencies: [idep_vulkan_runtime, idep_mesautil, dep_thread],
    c_args: [no_override_init_args, '-UNDEBUG'],
    link_args: [
      '-Wl,--wrap=kbase_csf_submit', '-Wl,--wrap=kbase_csf_queue_ref',
      '-Wl,--wrap=kbase_csf_queue_unref', '-Wl,--wrap=kbase_csf_queue_seqno_va',
      '-Wl,--wrap=kbase_csf_queue_published_target', '-Wl,--wrap=kbase_csf_wait',
      '-Wl,--wrap=calloc',
    ],
  )
  test('panvk_kbase_csf_submit', kbase_submit_test,
       suite: ['pan-kmod-host'], timeout: 30)
  kbase_compute_queue_test = executable(
    'panvk_kbase_compute_queue_test',
    [files('tests/panvk_kbase_compute_queue_test.c', 'panvk_kbase_sync.c',
           'panvk_vX_kbase_csf_encode.c'), panvk_entrypoints[0], panvk_tracepoints[0]],
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('csf')],
    dependencies: [idep_vulkan_runtime, idep_vulkan_wsi_headers, idep_mesautil,
                   idep_pan_packers, idep_nir_headers, dep_libdrm, dep_thread],
    c_args: [no_override_init_args, '-UNDEBUG', '-DPAN_ARCH=13'],
    link_args: [ld_args_gc_sections, '-Wl,--wrap=vk_sync_wait_many',
                '-Wl,--wrap=_vk_queue_set_lost'],
  )
  test('panvk_kbase_compute_queue', kbase_compute_queue_test,
       suite: ['pan-kmod-host'], timeout: 30)
  kbase_queue_init_test = executable(
    'panvk_kbase_queue_init_test',
    [files('tests/panvk_kbase_queue_init_test.c', 'panvk_vX_kbase_csf_encode.c'),
     panvk_entrypoints[0], panvk_tracepoints[0]],
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('csf')],
    dependencies: [idep_vulkan_runtime, idep_vulkan_wsi_headers, idep_mesautil,
                   idep_pan_packers, idep_nir_headers, dep_libdrm, dep_thread],
    c_args: [no_override_init_args, '-UNDEBUG', '-DPAN_ARCH=13',
             '-ffunction-sections', '-fdata-sections'],
    link_args: [ld_args_gc_sections, '-Wl,--wrap=_vk_queue_set_lost'],
  )
  test('panvk_kbase_queue_init', kbase_queue_init_test,
       suite: ['pan-kmod-host'], timeout: 30)
  kbase_cmd_draw_test = executable(
    'panvk_kbase_cmd_draw_test',
    [files('tests/panvk_kbase_cmd_draw_test.c', 'csf/panvk_vX_instr.c'),
     panvk_entrypoints[0], panvk_tracepoints[0], panvk_tracepoints[2]],
    include_directories: [inc_include, inc_src, inc_panfrost,
                          include_directories('csf', 'fifthgen')],
    dependencies: [idep_vulkan_runtime, idep_vulkan_wsi_headers, idep_mesautil,
                   idep_pan_packers, idep_nir_headers, dep_libdrm, dep_thread],
    link_with: [libpanfrost_lib, libpanfrost_shared],
    c_args: [no_override_init_args, '-UNDEBUG', '-DPAN_ARCH=13',
             '-ffunction-sections', '-fdata-sections'],
    link_args: [ld_args_gc_sections],
  )
  test('panvk_kbase_cmd_draw', kbase_cmd_draw_test,
       suite: ['pan-kmod-host'], timeout: 30)
  kbase_device_test = executable(
    'panvk_kbase_device_test',
    [files('tests/panvk_kbase_device_test.c', 'panvk_kbase_device.c',
           'panvk_kbase_sync.c'), panvk_entrypoints[0], panvk_tracepoints[0]],
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('csf')],
    dependencies: [idep_vulkan_runtime, idep_vulkan_wsi_headers, idep_mesautil,
                   idep_pan_packers, idep_nir_headers, dep_libdrm, dep_thread],
    c_args: [no_override_init_args, '-UNDEBUG', '-DPAN_ARCH=13',
             '-ffunction-sections', '-fdata-sections'],
    link_args: [ld_args_gc_sections, '-Wl,--wrap=open', '-Wl,--wrap=open64',
                '-Wl,--wrap=close'],
  )
  test('panvk_kbase_device', kbase_device_test,
       suite: ['pan-kmod-host'], timeout: 30)
  kbase_runtime_test = executable(
    'panvk_kbase_runtime_test',
    [files('tests/panvk_kbase_runtime_test.c', 'panvk_kbase_device.c',
           'panvk_kbase_sync.c', 'panvk_kbase_semaphore.c'), panvk_entrypoints[0], panvk_tracepoints[0]],
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('csf')],
    dependencies: [idep_vulkan_runtime, idep_vulkan_wsi_headers, idep_mesautil,
                   idep_pan_packers, idep_nir_headers, dep_libdrm, dep_thread],
    c_args: [no_override_init_args, '-UNDEBUG', '-DPAN_ARCH=13',
             '-ffunction-sections', '-fdata-sections'],
    link_args: [ld_args_gc_sections],
  )
  test('panvk_kbase_runtime', kbase_runtime_test,
       suite: ['pan-kmod-host'], timeout: 30)
  kbase_physical_test = executable(
    'panvk_kbase_physical_device_test',
    [files('tests/panvk_kbase_physical_device_test.c', 'panvk_vX_physical_device.c',
           'panvk_kbase_device.c', 'panvk_kbase_sync.c'),
     panvk_entrypoints[0], panvk_tracepoints[0], panvk_drirc[1]],
    include_directories: [inc_include, inc_src, inc_panfrost,
                          include_directories('csf', 'fifthgen')],
    dependencies: [idep_vulkan_runtime, idep_vulkan_wsi_headers, idep_mesautil,
                   idep_pan_packers, idep_nir_headers, dep_libdrm, dep_thread],
    link_with: [libpanfrost_lib, libpanfrost_shared],
    c_args: [no_override_init_args, '-UNDEBUG', '-DPAN_ARCH=13',
             '-ffunction-sections', '-fdata-sections'],
    link_args: [ld_args_gc_sections, '-Wl,--wrap=panvk_kbase_open_device',
                '-Wl,--wrap=pan_kmod_dev_create', '-Wl,--wrap=pan_kmod_dev_destroy',
                '-Wl,--wrap=vk_drm_syncobj_get_type_from_provider',
                '-Wl,--wrap=pan_get_compiler_flags', '-Wl,--wrap=kbase_kmod_ops',
                '-Wl,--wrap=open', '-Wl,--wrap=open64', '-Wl,--wrap=ioctl',
                '-Wl,--wrap=mmap', '-Wl,--wrap=mmap64',
                '-Wl,--wrap=disk_cache_create'],
  )
  test('panvk_kbase_physical_device', kbase_physical_test,
       suite: ['pan-kmod-host'], timeout: 30)
  kbase_instance_test = executable(
    'panvk_kbase_instance_test',
    [files('tests/panvk_kbase_instance_test.c', 'panvk_kbase_device.c',
           'panvk_kbase_sync.c', '../lib/pan_trace.c'),
     panvk_entrypoints[0], panvk_tracepoints[0], panvk_drirc[1]],
    include_directories: [inc_include, inc_src, inc_panfrost, include_directories('csf')],
    dependencies: [idep_vulkan_runtime, idep_vulkan_wsi_headers, idep_mesautil,
                   idep_pan_packers, idep_nir_headers, dep_libdrm, dep_thread, idep_xmlconfig],
    c_args: [no_override_init_args, '-UNDEBUG', '-DPAN_ARCH=13',
             '-ffunction-sections', '-fdata-sections'],
    link_args: [ld_args_gc_sections, ld_args_build_id],
  )
  test('panvk_kbase_instance', kbase_instance_test,
       suite: ['pan-kmod-host'], timeout: 30)
  if host_machine.system() == 'linux' and host_machine.cpu_family() in ['aarch64', 'x86_64']
    kbase_icd_test = executable(
      'panvk_kbase_icd_test',
      files('tests/panvk_kbase_icd_test.c', 'tests/panvk_kbase_icd_guard.c'),
      include_directories: [inc_include, inc_src, inc_panfrost],
      dependencies: [idep_pan_packers, idep_mesautil, dep_dl, dep_thread],
      c_args: [no_override_init_args, '-UNDEBUG', '-U_FILE_OFFSET_BITS', '-DPAN_ARCH=13'],
      link_args: ['-Wl,--export-dynamic'],
    )
    foreach case : ['success', 'reopen', 'profile', 'csif', 'jit', 'allocation',
                    'register', 'group', 'bind', 'heap', 'alias', 'kick',
                    'completion-error', 'host-oom-1', 'host-oom-4', 'host-oom-12',
                    'map', 'stop', 'heap-term', 'completion-timeout']
      test('panvk_kbase_icd_' + case.underscorify(), kbase_icd_test,
           args: [libvulkan_panfrost.full_path(), case],
           depends: [libvulkan_panfrost], suite: ['pan-kmod-host'], timeout: 30)
    endforeach
    kbase_enum_probe = executable(
      'panvk_kbase_enum_probe',
      files('tests/panvk_kbase_enum_probe.c', 'tests/panvk_kbase_icd_guard.c'),
      include_directories: [inc_include, inc_src, inc_panfrost],
      dependencies: [idep_mesautil, dep_dl, dep_thread],
      c_args: [no_override_init_args, '-UNDEBUG', '-U_FILE_OFFSET_BITS'],
      link_args: ['-Wl,--export-dynamic'],
    )
    test('panvk_kbase_enum_probe_cpu', kbase_enum_probe,
         args: ['--cpu', libvulkan_panfrost.full_path()],
         depends: [libvulkan_panfrost], suite: ['pan-kmod-host'], timeout: 25)
    foreach fixture : ['positive', 'count-change', 'bad-sum', 'nonuniform', 'scoreboard']
      test('panvk_kbase_csf_probe_' + fixture.underscorify(), kbase_enum_probe,
           args: ['--cpu-csf', libvulkan_panfrost.full_path(), fixture],
           depends: [libvulkan_panfrost], suite: ['pan-kmod-host'], timeout: 25)
    endforeach
    foreach fixture : ['positive', 'allocation-fail', 'mapping-fail', 'clean-fail',
                       'invalidate-fail', 'invalid-output', 'invalid-flags', 'free-fail', 'unmap-fail']
      test('panvk_kbase_memory_probe_' + fixture.underscorify(), kbase_enum_probe,
           args: ['--cpu-memory', libvulkan_panfrost.full_path(), fixture],
           depends: [libvulkan_panfrost], suite: ['pan-kmod-host'], timeout: 25)
    endforeach
    foreach fixture : ['positive', 'allocation-fail', 'mapping-fail', 'clean-fail',
                       'invalidate-fail', 'invalid-output', 'invalid-flags', 'free-fail',
                       'unmap-fail', 'missing-event']
      test('panvk_kbase_event_probe_' + fixture.underscorify(), kbase_enum_probe,
           args: ['--cpu-event', libvulkan_panfrost.full_path(), fixture],
           depends: [libvulkan_panfrost], suite: ['pan-kmod-host'], timeout: 25)
    endforeach
    foreach fixture : ['positive', 'different-cookie', 'backing-allocation-fail', 'backing-mapping-fail',
                       'backing-invalid-cookie', 'backing-cached', 'alias-ioctl-fail',
                       'alias-mapping-fail', 'alias-invalid-cookie', 'alias-invalid-pages',
                       'alias-cached', 'alias-missing-need-mmap', 'alias-same-va-only', 'alias-unmap-fail',
                       'alias-free-fail', 'backing-unmap-fail', 'backing-free-fail',
                       'cross-window', 'view-mismatch', 'backing-guard-fail', 'alias-guard-fail']
      test('panvk_kbase_alias_probe_' + fixture.underscorify(), kbase_enum_probe,
           args: ['--cpu-alias', libvulkan_panfrost.full_path(), fixture],
           depends: [libvulkan_panfrost], suite: ['pan-kmod-host'], timeout: 25)
    endforeach
    test('panvk_kbase_device_probe_cpu', kbase_enum_probe,
         args: ['--cpu-device', libvulkan_panfrost.full_path(), 'positive'],
         depends: [libvulkan_panfrost], suite: ['pan-kmod-host'], timeout: 25)
  endif
endif
