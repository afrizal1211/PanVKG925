# GPU 驱动台账 (Driver Ledger)

> 本文件是**持久化事实源**，不随对话压缩而丢失。
> 由 SessionStart hook 自动注入回上下文（startup / resume / clear / **compact**）。
> 规则：**动手改驱动代码前先读本文件；发现新的寄存器/MMIO/中断/调试事实就立刻追加进来。**

## 维护协议

- **只追加，不改写历史条目**。发现旧条目有误 → 在下方「更正记录」写一条，不要静默修改。
- 每条事实写清 **来源**（文件:行号 / 命令输出 / 寄存器 dump）和 **确认状态**（`已验证` / `待验证` / `已推翻`）。
- 反直觉的坑（"明明看着对但就是不对"）优先记，这里比代码注释更抗压缩。

---

## 项目基线

> 来源：`mesa-mtk/CLAUDE.md`（2026-10-15 快照）。**本仓库代码比该快照更新，基线待核实。**

- 本 fork：在 Panfrost/PanVK 上增加 Arm 私有 `kbase` 内核驱动后端（`/dev/mali*`，r32p0–r44p0），替代上游 `panfrost`/`panthor` DRM 驱动。
- 目标设备：Google Pixel 7，Mali-G710，Valhall/CSF，arch ≥ 10。
- 构建关键选项：`-Dpanfrost-kmds=kbase,panthor`；每个启用的 KMD 定义 `HAVE_PAN_KMOD_<NAME>`，
  所有 kbase 专属代码必须留在 `#if defined(HAVE_PAN_KMOD_KBASE)` 后面。
- kmod 抽象层：`src/panfrost/lib/kmod/`（`pan_kmod.c` / `pan_kmod_backend.h` / `kbase_kmod.c`）。
- **待核实**：本仓库已出现 `kbase_csf.c`、`kbase_sync.c`、`kmod/tests/`、`kmod/virtio/`，快照文档未覆盖。

---

## 寄存器定义

> 格式：`名称 | 偏移/地址 | 宽度 | 读写属性 | 语义 | 来源 | 状态`

| 寄存器 | 偏移 | 宽度 | 属性 | 语义 | 来源 | 状态 |
|---|---|---|---|---|---|---|
| _(待填充)_ | | | | | | |

---

## MMIO 映射

> 格式：`区域 | 物理基址 | 大小 | 用途 | 证据来源 | 状态`

| 区域 | 基址 | 大小 | 用途 | 证据来源 | 状态 |
|---|---|---|---|---|---|
| _(待填充)_ | | | | | |

---

## 中断流程

> 格式：`IRQ/信号 | 触发条件 | 处理路径 | 同步原语 | 坑 | 状态`

| IRQ / 信号 | 触发条件 | 处理路径 | 同步原语 | 已知坑 | 状态 |
|---|---|---|---|---|---|
| _(待填充)_ | | | | | |

---

## 调试发现

> 只记"反直觉"和"踩过两次以上"的。按发现日期倒序追加。

| 日期 | 发现 | 根因 | 证据 | 状态 |
|---|---|---|---|---|
| 2026-10-16 | 真机 device 探针 `vkEnumeratePhysicalDevices` 回 `VK_SUCCESS` 但 **0 设备**，且 `version_calls=0`（**kbase ioctl 一次都没发**） | 探针 harness 的 open 白名单漏放行物理 `/dev/mali0` handoff；`panvk_kbase_open_device` 失败后 `panvk_kbase_enumerate_physical_device` 把 `VK_ERROR_INCOMPATIBLE_DRIVER` **静默映射**成 `VK_SUCCESS` + 0 设备 | `m5a-20261016-194448/live-device-2.log`（`count=0 blocked=3 version_calls=0`）；`panvk_kbase_device.c:69`、`panvk_physical_device.c:541` | 已验证（已修，`live-device-3.log` 通过） |
| 2026-10-16 | **"枚举回 0 设备"不是内核/kbase ABI 问题**，别往驱动方向查 | 判据是 `version_calls`：为 0 且无 `DEVICE_VERSION` 行 ⇒ 失败在 **open/包装层**，在首次 ioctl 之前 | 同上 | 已验证 |
| 2026-10-16 | 探针 device 模式的 reopen 白名单与 `handed_off` 标志**耦合**，只修 open 不够 | `handed_off` 是 ioctl wrapper 的前置条件（`probe.active && probe.handed_off && arg`），reopen 分支不置它则后续 ioctl 全被拒 | `panvk_kbase_enum_probe.c` ioctl wrapper；`live-device-2.log` | 已验证（已修） |
| 2026-10-16 | `manifest.json` 被误判"三处哈希全过期"，实为归档约定误读 | `manifest.json` 记**阶段开始前**（`original/`），`current-manifest.json` 记**阶段结束后**（`current/`）；误判者拿阶段前清单对阶段后工作树 | m4g 全部六条哈希逐一核对：`manifest.json`↔`original/` 全符，`current-manifest.json`↔`current/` 全符 | 已验证（见 `OPEN-ITEMS.md` 更正记录 C1） |
| 2026-10-17 | **ICD 级探针的 mock 会用 `blocked()` 静默吸收驱动多发的 ioctl**，于是想当然以为"fixture 通过 ⇒ 代码正确" | 走 `blocked()` 的分支直接回 `-1/EPERM`，驱动侧观察不到差异。M4g 的 `alias-invalid-*` fixture 对着**会发 `MEM_FREE` 的旧代码**照样通过 | 旧代码下 `panvk_kbase_alias_probe_alias_invalid_cookie` 等全部 OK（97/97 里含它们）；真正的 oracle 是 `kbase_kmod_test.c` 的 `__wrap_*` 计数器 —— 回退修复后 `kbase_kmod_test.c:861` abort | 已验证 |
| 2026-10-17 | **ICD 内部符号探针读不到**（`-fvisibility=hidden`） | `nm -D libvulkan_panfrost.so` 只导出 `vk_icdGetInstanceProcAddr`/`vk_icdGetPhysicalDeviceProcAddr`/`vk_icdNegotiateLoaderICDInterfaceVersion` 三个符号；探针是独立进程，只经 syscall 观测。驱动内部状态（如 `alias_cleanup_uncertain`）无法直接读取，必须落到**可观测的 syscall 等价物**（munmap 返回值） | `nm -D --defined-only` 输出 3 行；`panvk_kbase_enum_probe.c` 用 `dlsym(icd,"vk_icdGetInstanceProcAddr")` 起手 | 已验证 |
| 2026-10-17 | **探针把失败的 unmap 记成成功** | `panvk_kbase_device_probe.inc` 的 `device_munmap` 修前**无条件** `unmaps++` / `entry->live=false`，失败的 munmap 也被计为已释放 → 恰好掩盖 cross-4GiB 泄漏 | M4g 负例 `cross-window`（`cpu_boundary_fixture=1`）只验窗口判定，走不到 unmap 失败分支 | 已验证（已修：仅成功后记账，失败打印 `DEVICE_UNMAP_FAILED` 并置 `cleanup_uncertain`） |
| 2026-10-17 | **给探针加"CPU 仿真"分支时，台账记账（`device_note`）必须放在 `probe.live` 早返回之前** —— 否则 live 路径永远不计该请求 | M5b-1 给 `panvk_kbase_device_probe.inc` 的 `CS_GET_GLB_IFACE` 加 CPU 仿真时，把 `probe.live ? host_ioctl(...)` 放在 `device_note(DEV_CSIF)` 前面返回 ⇒ live 不计 DEV_CSIF ⇒ `device_check_result` 的 `DEV_CSIF >= 1` 失败 ⇒ live `--live-device` 打 `csif=0 accepted=0`。**CPU 完全跑不出这个症状**（CPU 走的恰好是会记账的仿真分支） | `m5b1-.../logs/live-device-precheck-regression-csif0.log`（`csif=0 accepted=0`，exit=1）对比 `logs/live-device-baseline.log`（修后 `csif=2 accepted=1`，exit=0，与 M5a 基线一致） | 已验证（已修） |
| 2026-10-17 | **kbase ioctl union 的 `in` 与 `out` 在偏移 0 处重叠**（`cs_tiler_heap_init`、`mem_alias`、`cs_queue_bind` 都是）—— 写 `out` 会摧毁 `in` | union 不是 struct；诊断 printf 若要打印输入，**必须先把输入抓进局部变量**再写 out。本阶段被这个坑骗出 `nents=256 chunk=3411968` 这种垃圾值 | `mali_kbase_ioctl.h` 的 union 定义；本阶段 `DEVICE_ALIAS` / `DEVICE_HEAP_INIT` 诊断修复前后 | 已验证（诊断假象，非驱动 bug） |
| 2026-10-17 | **空 PRIMARY 命令缓冲在 kbase 上不 vacuous**：`EndCommandBuffer` 的空子队列快捷路径**显式排除** COMPUTE（`i != PANVK_SUBQUEUE_COMPUTE`）⇒ COMPUTE 永远走 `finish_cs` ⇒ 无条件发 `cs_wait_slots` + CS-error save + `cs_flush_caches` + `cs_wait_slot` ⇒ `cs_root_chunk_size != 0` ⇒ 真发 kick | 且只有 COMPUTE 有 chunk ⇒ `graphics` 保持 false ⇒ 不需要 tiler heap rearm、不撞 `graphics_ready`。注意 `cs_is_valid(b)` 只是 `!b->invalid`，对零命令缓冲**恒真** —— 真正的判据是 `cs_root_chunk_size`，不是那个 `cs_is_valid` 门 | `panvk_vX_cmd_buffer.c:271-283`；真机 `live-submit.log` 的 `CS_QUEUE_KICK ring_va=0x5ffffc1000 index=4 ret=0`；回退该条件 ⇒ `panvk_kbase_icd_test.c:924` abort 且 `--cpu-submit` 转 `kick_delta=0` | 已验证 |
| 2026-10-17 | 两个生产 hazard：非法 alias 输出**任意 `MEM_FREE`**；cross-4GiB **unmap 失败静默返回坏 VA** | 前者绑定条件是**来源（provenance）**，未通过校验的输出无法确立 cookie 归属 ⇒ 该分支**永不该 FREE**；后者 `alias->gpu_va` 本身就是坏 VA，且唯一调用点只看 NULL ⇒ 只能**回滚+上报**（不填 alias、不取 BO 引用） | `kbase_kmod.c:762`（`!valid`）/ `:785`（munmap 失败）；调用点 `panvk_kbase_compute_queue.inc:159` | 已验证（已修，`h1-*`/`h2-*` 检查点，回退即 abort） |

| 2026-10-18 | `perf_create` 是 `ENOTSUP` 空桩，但 `perf_enable/dump/destroy` **未设置**，而 `pan_kmod_perf_enable/dump/destroy()` **无条件解引用** `dev->ops->perf_*` | 一旦有人只把 `perf_create` 实现成返回非 NULL，`pan_kmod_perf_enable()` 立即空指针调用崩溃。当前不可达仅因为 `perf_create` 返回 NULL —— **不是**因为 vtable 完整 | `pan_kmod.h:1030` `:1043` `:1049`（三处均无 null 检查，对比 `:724` `:781` `:817` 的可选 ops 有检查）；`kbase_kmod.c` vtable 缺这 3 项 | 已验证（静态）；**改 perf 前必须先补齐这 3 个 op 或加门** |
| 2026-10-18 | 厂商 kbase **没有 dma-buf exporter** ⇒ `bo_export` 在本内核上**不可能**实现（不是没做，是做不了） | 全 vendor 树 `grep dma_buf_export` 只命中 `drivers/base/arm/dma_buf_test_exporter/`（ARM 测试模块），`mali_kbase` 零命中；`kbase_mem_import` 只处理 **import** 方向（UMM type 2 / USER_BUFFER type 3），无对称 export 路径 | `mt6991-kernel-src/mali-r49p1/drivers/gpu/arm/midgard/mali_kbase_mem_linux.c:2103`；全树 grep | 已验证（静态 grep，2026-10-18） |
| 2026-10-18 | `query_timestamp` 即使接 `GET_CPU_GPU_TIMEINFO`(nr 50) 也**缺 cycles→ns 的换算来源** | vendor uapi 只有 `BASE_TIMEINFO_{MONOTONIC,TIMESTAMP,CYCLE_COUNTER}_FLAG`，**没有** timestamp frequency 这个 gpuprop 键；`grep -rn "TIMESTAMP_FREQUENCY\|TIMESTAMP_PERIOD"` 在 midgard 全树零命中。panthor 的 frequency 来自 panthor 自己的 props，kbase 无对应物 | 同上 grep；`pan_kmod.h:256-259` 需要 `timestamp_frequency` 才能算 `timestamp_cycles_to_ns_factor` | 待验证（需 RE 或固定假设，别直接抄 panthor） |

| 2026-10-18 | **`kbase_mem_import` 有两条返回形状**，只处理 cookie 分支会漏掉一半：`(*flags & (SAME_VA\|NEED_MMAP)) != 0` ⇒ 返回 cookie（需 mmap 才进 GPU VA 空间）；**否则内核自己 `kbase_add_va_region`/`kbase_gpu_mmap` 并返回真实 GPU VA** | 反编译 `kbase_mem_import`：分支判据是 `(*param_7 & 0x6000) == 0` ⇒ `add_va_region`/`gpu_mmap`，返回 `reg->start_pfn << 12`；`else` 走 `pending_regions` cookie 路径，返回 `slot*0x1000 + 0x40000`。64 位客户端因 `kbase_mem_from_umm` 强制 NEED_MMAP **总是** cookie 分支，但内核 ABI 两条都存在 | `mali_kbase_mt6991_r49.ko` @ `kbase_mem_import`（REA/Ghidra 伪代码，发行版二进制）；回调 oracle：只按 NEED_MMAP 判定 ⇒ `kbase_kmod_test` 在 `direct != NULL` 处 abort | 已验证（已修，两条分支都实现） |
| 2026-10-18 | imported UMM 的 **CPU 映射必须 `PROT_NONE`** —— 这是 mmap 一致性检查的**唯一可通过形状** | `kbase_context_mmap` 的检查是 `(reg CPU_RD \|\| !VM_READ) && (reg CPU_WR \|\| !VM_WRITE)`。`PROT_NONE` 两个条件恒真；`PROT_READ` 要求 import flags 带 `CPU_RD`，而那样也毫无用处 —— fault handler 对 `IMPORTED_UMM && free_on_close` 直接 SIGBUS | 反编译 `kbase_context_mmap` @ `0x1509d0` 的 `LAB_00150b38`（`inconsistent VM flags`）；`kbase_cpu_vm_fault` @ `0x15032c` 首分支 `plVar9[10]==1 && *(uVar10+0x2c)!=0` ⇒ warn 后 `iVar3=2` = VM_FAULT_SIGBUS | 已验证（静态，发行版 `.ko`） |
| 2026-10-18 | 发行版 `.ko` **确认无 dma-buf exporter**，但**有完整 importer** | `nm` 里 `dma_buf_get/attach/detach/map_attachment_unlocked/put` 全是 `U`（外部引用），无 `dma_buf_export`；同时 `kbase_api_mem_import`、`kbase_mem_import`、`kbase_mem_umm_map(_attachment)` 都存在。⇒ `bo_export` 保持 `ENOTSUP` 是**内核能力边界**，不是欠账 | `nm mali_kbase_mt6991_r49.ko`（10 个 dma_buf 符号，全 `U`）；`kbase_mem_import` 的 callees 含 `dma_buf_get`/`dma_buf_attach`/`kbase_mem_umm_map_attachment` | 已验证（发行版二进制，非仅源码） |

| 2026-10-18 | **真机 `MEM_IMPORT` 实测（首次）**：4 KiB dma-buf（`/dev/dma_heap/system`）导入成功，**返回 cookie `0x41000`**（cookie 窗口内），`va_pages=1`，`flags=0x500c`，`same_va=0`、`need_mmap=1`。`mmap(PROT_NONE, 4096, MAP_SHARED, fd, 0x41000)` 成功，`munmap` ret=0。连跑 4 次结果**逐字节一致**，dmesg 零错误 | 静态预测的五条全部命中；`0x500c` = `NEED_MMAP(0x4000)｜CACHED_CPU(0x1000)｜CPU_RD(0x1)｜GPU_RD(0x4)｜GPU_WR(0x8)` | `import-probe/live-import.log`，探针源码 `import-probe/kbase_import_probe.c` | 已验证（真机，PLC110，kbase 1.30） |
| 2026-10-18 | **更正上一条 `PROT_NONE` 的理由**：内核**确实**强制加了 `CPU_RD`（`0x500c` 含 bit0），所以 `PROT_READ` 映射**能过**一致性检查；挡住的只是 `PROT_WRITE`（`CPU_WR` 未给）。`PROT_NONE` 仍是最稳妥选择，但**不是唯一能通过的形状** —— 原记录说"唯一"是错的 | 实测 `flags=0x500c`；`kbase_mem_from_umm` 强制 `CACHED_CPU` + `kbase_update_region_flags` 把 `PROT_CPU_RD` 映成 reg `CPU_RD` | 同上 live-import.log | 已验证（更正 static-only 推断） |
| 2026-10-18 | **`MEM_IMPORT` 错误码反直觉**：非法 `type`（`0xdead`）返回 **`ENOMEM(12)`**，不是 `EINVAL` | 内核 `kbase_mem_import` 的 `default:` 分支 `reg = NULL; goto no_reg`，`no_reg` 统一 `return -ENOMEM` | live-import.log 的 `CONTROL bad_type ret=-1 errno=12`；与反编译的 `LAB_0014fd00: uVar15 = 0xfffffff4`（-12）一致 | 已验证（真机 + 静态吻合） |
| 2026-10-18 | **多尺寸 `MEM_IMPORT` 扫描 8/8 全过**：4 KiB / 8 KiB / 16 KiB / 64 KiB / 256 KiB / 1 MiB / 2 MiB / 8 MiB，每个都返回**同一个 cookie `0x41000`**（cookie 归还后复用，与 M4g 的槽位复用语义一致），`va_pages` 随尺寸 1→2048 线性增长，`PROT_NONE` placement mmap 与 `munmap` 全部成功 | 每次 import 单独分配/释放，失败可归因到尺寸 | `import-probe/live-import-sizes.log` | 已验证（真机） |
| 2026-10-18 | **`0x500c` 的正确解码**：`NEED_MMAP(bit14) \| CACHED_CPU(bit12) \| GPU_RD(bit2) \| GPU_WR(bit3)`。**`CPU_RD(bit0)` 与 `CPU_WR(bit1) 都是 0`** —— 内核**没有**给 CPU 权限 | `kbase_mem_from_umm` 只强制 `CACHED_CPU`；`kbase_update_region_flags` 不会凭空加 CPU 位 | 真机 `flags=0x500c`；uapi `BASE_MEM_CACHED_CPU = 1<<12`、`CPU_RD = 1<<0` | 已验证（真机 + uapi 对照） |

## 更正记录

| 日期 | 原条目 | 更正为 | 依据 |
|---|---|---|---|
| 2026-10-18 | 2026-10-18「更正上一条 `PROT_NONE` 的理由」条目称：**"内核确实强制加了 `CPU_RD`（`0x500c` 含 bit0），所以 `PROT_READ` 映射能过一致性检查"** | **该更正本身是错的。** `0x500c` 的 bit0 = **0**，`CPU_RD` 与 `CPU_WR` **都没给**。因此 **`PROT_NONE` 确实是唯一能过 `kbase_context_mmap` 一致性检查的形状**（`CPU_RD \|\| !VM_READ` 与 `CPU_WR \|\| !VM_WRITE` 只有 VM_READ/VM_WRITE 都为假时才恒真）。原始 static 推断（`ab-20261018` rev2 的 "hard_constraint_2"）是对的，是这次真机解读算错了位。 | `grep 'BASE_MEM_CACHED_CPU' include/drm-uapi/mali_kbase_ioctl.h` → `1 << 12`；`0x500c & 1 == 0`；真机日志 `cpu_rd=0 cpu_wr=0 cached=1`；探针打印用的是正确的 uapi 宏，错的是台账解读 |

| 2026-10-18 | **alias 第二段对 GPU 可达**：带 `SYNC_ADD64 @ alias_va+512KiB` 的 compute 流真机提交返回 `VK_SUCCESS` 且 `vkQueueWaitIdle` 返回 `VK_SUCCESS`（`accepted=1 blocked=0 gpu_completion_observed=1`）。因 `kbase_compute_execute` 把任何提交/完成失败都映射成 `vk_queue_set_lost`，这说明**第二段在 GPU 页表里是映射的**——未映射会 fault 并置 lost | GPU 页表覆盖 `stride*nents`，不只是首段。**注意：这只证明"可寻址"，未证明"写入的数据正确"**（没有回读） | `c-20261018-084340/logs/live-alias-writeback.log`；3 次运行全 exit=0，无 panic/黑屏 | 已验证（真机，可达性）；**数据正确性未证明** |
| 2026-10-18 | **kbase 路径从不设置 `render_desc_ringbuf.addr.host`**：`panvk_vX_gpu_queue.c:112` 只在 DRM 路径设它；`kbase_graphics_resources()` 自建 ring BO，不设。于是 `panvk_vX_gpu_queue.c:76` 的 teardown 分支在 kbase 上**永远不执行** | 两条路径各自建 ring，kbase 分支漏了对应字段 | `grep -n addr.host src/panfrost/vulkan/csf/panvk_kbase_compute_queue.inc`（只有注释命中，无赋值）；`panvk_vX_gpu_queue.c:76,112` | 已验证（静态）；**未修** |
| 2026-10-18 | **探针环境下 mesa 日志整体不可用**：`PANVK_DEBUG=startup` 不打印 `panvk_debug:` 行，驱动内 `mesa_logi` 与裸 `fprintf(stderr)` 均无输出，而探针自己的 `printf` 正常。guard 是默认 `DENY` 白名单且不含 `write`，但 `printf` 能出说明不全是 guard 的锅 | 未定 | 多次 `--live-submit` 尝试 | **未解决**（阻塞基于日志的回读方案） |

| 2026-10-18 | ⭐ **alias 环形窗口闭环证明**：GPU 写 `alias_va+512KiB` 的值被 CPU 从 backing 映射**精确读回**（`expected=0x1122334455667788 observed=0x1122334455667788 match=1`，两次）。⇒ 第二段解析到**预期的物理页**（= 唯一 backing BO 的起点），不只是"能寻址" | alias 的 `nents=2 × stride=512KiB` 确实是**同一块物理内存的两份视图**，GPU 侧按 1 MiB 环形窗口使用是成立的 | `a-20261018-085944/logs/a-live-alias-readback.log`；oracle：写地址 +8 ⇒ 全部 match=0、exit=1 | 已验证（真机，生产几何 512KiB×2） |
| 2026-10-18 | **探针会主动 `unsetenv("PANVK_DEBUG")`**（`panvk_kbase_enum_probe.c:1370`），这是为了基线可复现的**故意设计**。任何经 `PANVK_DEBUG` 门控的驱动侧调试在探针里**永远看不到** | 探针固定环境后 `dlopen` ICD；`panvk_debug_init` 在 ICD 的 `CreateInstance` 里才跑，读到的是被清空后的 environ | 修复前 `KBASE_DBG_INIT PANVK_DEBUG=(null) panvk_debug=0x0`；改经 `PANVK_KBASE_PROBE_DEBUG` 转发后 `panvk_debug=0x100000` | 已验证（已修：探针转发 `PANVK_KBASE_PROBE_DEBUG`→`PANVK_DEBUG`，默认仍隔离） |
| 2026-10-18 | **kbase teardown 安全**：`destroy_gpu_queue()` 走 `panvk_device_uses_kbase()` 分支调 `kbase_compute_cleanup()`，**不经过** `cleanup_queue()`/`finish_render_desc_ringbuf()` ⇒ 那个会在 kbase 上误调 `panvk_as_free(alias_va)` 的路径**根本不可达**（该 VA 从未经 `panvk_as_alloc`） | 两条 destroy 路径分离 | `panvk_vX_gpu_queue.c:1520-1537` 的早期 return；`kbase_compute_cleanup` 全文 | 已验证（静态 + 97/97 回归） |
| 2026-10-18 | **`addr.host` 在 kbase 上是"借来的只读视图"**，不能参与释放：backing 映射归 `kbase_kmod_bo_free()`（`munmap(cpu_ptr)`），alias 映射归 `kbase_kmod_alias_destroy()`（`munmap(mapping)`）。从 `addr.host` 再 munmap 会 **double-munmap** | 两处各自拥有自己的映射 | `kbase_kmod.c` 的 `bo_free` / `alias_destroy`；`kbase_compute_cleanup` 里显式清空 `addr.host` | 已验证（已按此实现） |

| 2026-10-18 | **kbase 绘路径跳过的 5 处 heap 指令点**：全部由 `cmdbuf_skips_gpu_heap_ops()`（`panvk_vX_cmd_draw.c:57`，等价 `panvk_device_uses_kbase()`）门控 —— `:4236` `:4254`（`cs_vt_end`）、`:4534`（tiler OOM ctx + handler）、`:4561`（关 handler）、`:4671/:4674`（`cs_finish_fragment`） | kbase 只发 `VERTEX_TILER_STARTED`，不发 COMPLETED/FINISH_FRAGMENT，因此 heap 统计恒为 started-only —— 这是**有意设计**，因为内核在 chunk-grow 时对 `frag_end > vt_end` 或全零计数都杀组 | `panvk_vX_cmd_draw.c` 五处 + `docs`；`first-frame-prep-20261018/EXPERIMENT-CARD.md` | 已验证（静态） |
| 2026-10-18 | **`panvk_kbase_compute_submit` 的 graphics 前置门控**：`graphics_ready` 不成立直接 `VK_ERROR_FEATURE_NOT_PRESENT`；成立则**先分配 replacement tiler heap** 再提交，并 rearm 两个 graphics 子队列的 noop stream | 首帧若报 `FEATURE_NOT_PRESENT`，先查这个门控，不是硬件问题 | `panvk_kbase_compute_queue.inc:358`（gate）、`:381-393`（rearm）、`:395+`（encode） | 已验证（静态） |
| 2026-10-18 | **单元测试断言要挑 fixture 真有值的字段**：`desc_alias` 与 `tiler_heap.context` 在 `panvk_kbase_compute_queue_test.c` 的那个位置分别是 NULL / 0，拿它们做断言恒真、**oracle 不挂**。改用 `has_tiler_heap`（该点已建 heap）后 oracle 立即成立 | "断言 == 常量" 与 "断言 == 未初始化的镜像" 同样无效 | 三次 oracle 尝试：`graphics_ready`(恒 true)、`desc_alias`(两边 false)、`tiler_heap_context`(两边 0) 都没挂；`has_tiler_heap` 挂 | 已验证 |
| 2026-10-18 | **meson 不把 `.inc` 当依赖**：改 `panvk_kbase_compute_queue.inc` 后 `ninja` 报 `no work to do`，测试仍是旧二进制 | `.inc` 被 `#include` 但不进依赖图 | 观察：改 `.inc` 后 ninja 无动作；`rm` 掉 `.o` 才重编 | 已验证（**改 `.inc` 后要 `rm` 对应 `.o` 或 `touch` 测试源**） |

| 2026-10-18 | ⭐ **U1 源码裁决：kbase 的 started-only heap 统计是安全的**。校验在 `mali_kbase_csf.c:2076`：`(frag_end > vt_end) \|\| (vt_end >= vt_start)` → 杀组。started-only 的 `vt_start=N, vt_end=0, frag_end=0` 代入**两条都不成立**；第二层 `validate_allocation_request()` 的 `WARN_ON(!nr_in_flight)`（`nr_in_flight = N`）也不触发 | 而且这段只在固件发 `CS_REQ.TILER_OOM` 时跑，kbase 跳过 `cs_set_exception_handler(TILER_OOM)` ⇒ **校验根本不会执行** | `mt6991-kernel-src/.../csf/mali_kbase_csf.c:2062-2100`、`:2200-2226`；`csf/mali_kbase_csf_tiler_heap.c` `validate_allocation_request` | 已验证（官方 GPL 源码） |
| 2026-10-18 | **`get_aliased_alloc` 是循环遍历，不是"只看首段"**：`while (*start_off >= stride) { aliased++; *start_off -= stride; }`。所以 offset==stride **能正确选到 entry[1]** | "alias CPU 映射只覆盖首段"是 **CPU fault 路径**的特有问题（调用处总传 VMA 起点），**GPU 路径不受影响** | `mt6991-kernel-src/.../mali_kbase_mem_linux.c:2530` | 已验证（源码） |
| 2026-10-18 | **imported UMM 在 import 时就建好 GPU 页表**：`kbase_mem_umm_map_attachment()` 立即 `dma_buf_map_attachment(DMA_BIDIRECTIONAL)` 并把 `sg_dma_address` 填进 `gpu_alloc`、设 `nents` | 前提是 `CONFIG_MALI_DMA_BUF_MAP_ON_DEMAND` 未开（源里 `if (!IS_ENABLED(...))` 分支）。**未验的只剩缓存一致性**（源码注释明说无法查询 exporter 的 CPU 缓存属性） | `mt6991-kernel-src/.../mali_kbase_mem_linux.c` 的 `kbase_mem_umm_map_attachment` | 已验证（源码） |
| 2026-10-18 | **KCPU 的 FENCE_SIGNAL 产出真 `dma_fence`**：`csf/mali_kbase_csf_kcpu.c:2025` `*sync_file = sync_file_create(fence_out);` ⇒ 内核侧有 timeline 语义的 fence，**是我们用户态没接**，不是内核做不到 | 影响 U7（timeline semaphore）的定性：属"没做"而非"做不了" | 同左 | 已验证（源码） |
| 2026-10-18 | **dma-heap 的 `fd_flags` 带 `O_RDWR` 时拒绝非页对齐长度（EINVAL）**，`O_RDONLY` 则接受。非页对齐 dma-buf（1/4095/4097/8191/12289）import 全部成功，`va_pages` 正确向上取整 | 探针改用 `O_RDONLY \| O_CLOEXEC` | 真机 `import-probe/live-import-unaligned.log`；`strace` 对照 | 已验证（真机） |

> M5a / M5b-1 真机实测硬件事实（设备身份、ioctl 观测、映射生命周期、用户提交的 kick）见
> `/home/zl/github/mesa-port-backups/RE-NOTES.md` §8 与 §10；本台账不重复抄录。

---

## 更正记录

| 日期 | 原条目 | 更正为 | 依据 |
|---|---|---|---|
| _(暂无)_ | | | |

| 2026-10-18 | ⭐ **`CQS_WAIT` / `FENCE_WAIT` 不阻塞内核线程**：`kbase_csf_kcpu_queue_process()` 是遍历 pending 命令的 for 循环，条件不满足时 `process_next = false` 后 `break` 出 switch，循环判 `process_next` 即返回 | **更正旧结论**"不可恢复地挂死内核 kcpu worker"。真实危害是该队列后续命令不推进 → 上层等不到 fence；`kill` 进程即触发 `kcpu_queue_context_term()` → `delete_queue(drain=true)` 清理 | `csf/mali_kbase_csf_kcpu.c:2593-2623`(FENCE_WAIT)、`:2645-2659`(CQS_WAIT)、`:3169-3178`(context_term) | 已验证（源码） |
| 2026-10-18 | **`CQS_WAIT` 的对象必须有 `reg->cpu_alloc->permanent_map`**：`kbase_phy_alloc_mapping_get()` 在 `permanent_map == NULL` 时返回 NULL | 而该映射只对 `KBASE_MEM_TYPE_NATIVE` 且带 `BASE_MEM_PERMANENT_KERNEL_MAPPING` 的区域建立；`BASE_MEM_CSF_EVENT` 会自动带上它 ⇒ **只有 CSF_EVENT BO 能当 CQS 对象，imported dma-buf 不行** | `mali_kbase_mem_linux.c:202-232`(init)、`:246-288`(get)；`mali_kbase_mem.c:1248-1251` | 已验证（源码） |
| 2026-10-18 | **内核在 `MEM_ALLOC_EX` 入口对 `CSF_EVENT` 强制补 flag**：`flags |= (BASE_MEM_SAME_VA \| BASE_MEM_CACHED_CPU \| BASE_MEM_COHERENT_SYSTEM)`，且拒绝 `gpu_executable`/`fixed_or_fixable` | ⇒ 客户端**不需要**自己加 `COHERENT_SYSTEM`；`KBASE_REG_SHARE_BOTH` 由内核置上，`WARN_ON(!SHARE_BOTH)` 不会触发 | `mali_kbase_core_linux.c:983-990`；Mesa 走 EX 见 `kbase_kmod.c:356-367` | 已验证（源码） |
| 2026-10-18 | **KCPU 入队是单一命令**：`if (enq->nr_commands != 1) return -EINVAL;`（Base 接口不暴露多命令） | 多命令顺序只能靠**多次入队 + 队列 FIFO**（`queue->commands[(u8)(start_offset + i)]`）。所以"先 CQS_WAIT 后 FENCE_SIGNAL"要分两次 ioctl | `csf/mali_kbase_csf_kcpu.c:2997`、`:3040-3046` | 已验证（源码） |
| 2026-10-18 | **`FENCE_SIGNAL` 在 prepare 阶段就建好 fd**：`dma_fence_init` + `sync_file_create` + `get_unused_fd_flags`，fd 写回用户态的 `base_fence.basep.fd`；命令**执行时**才 `dma_fence_signal()` | ⇒ 用户态在入队那一刻拿到 fd，它在此后的 CQS_WAIT 满足前一直未 signaled。**这正是"分两次入队"可行且正确的关键** | `csf/mali_kbase_csf_kcpu.c:1994-2042`(init)、`:2058-2085`(prepare)、`:1954`(signal 执行) | 已验证（源码） |

| 2026-10-18 | ⭐ **KCPU 在真机上是活的**：握手后 `KCPU_QUEUE_CREATE`(45)/`DELETE`(46) 成功；`FENCE_SIGNAL` 入队后**内核回写有效 sync_file fd**（fd=4），空闲队列上 `poll()` 立即返回 `POLLIN` | 但该 fence 由**命令被执行**驱动，不是 GPU 完成驱动。空闲队列上立即就绪是预期的，不能当作完成信号 | 真机 `kcpu-e1e3-20261018/{e1,e2}.log`，uapi 1.30，exit=0 | 已验证（真机） |
| 2026-10-18 | ⭐ **`CQS_WAIT` 只能被 GPU / `CQS_SET` / 错误路径放行，纯 CPU 写内存唤不醒它**：`event_cqs_callback` 注册在 `kctx->csf.event.callback_list`，该链表只由 `kbase_csf_event_signal()` 遍历；其调用点仅 `core_linux.c:1457`(ctx 创建)、`kcpu.c:983`(CQS_SET)、`:1304`(CQS_SET_OPERATION)、`:2848`(enqueue_failed)、`mali_kbase_csf.c:3749`(group 错误)、`mtk_platform_qinspect_recovery.c:179`。**没有任何"用户写内存"或定时轮询的路径**，`enqueue_kcpuq_work()` 也无周期性重试 | ⇒ 实验卡里"CPU 线程写内存释放 CQS_WAIT"的设计**与机制不符**。要验证串行性得用同队列 `CQS_SET`(E3a，A 级)；要验证 GPU 完成得用 `SYNC_ADD64`(E4，级 C 边界) | 真机 `kcpu-e1e3-20261018/e3.log` + `e3-retry.log`（两次完全一致，`blocked_before=1 released_after=0`）；源码 `csf/mali_kbase_csf_event.c:117-156`、`csf/mali_kbase_csf_kcpu.c:467-479,748-755,983,1304,2848` | 已验证（真机 + 源码） |
| 2026-10-18 | **`CQS_WAIT` 之后入队的 `FENCE_SIGNAL` 确实被挡住**：两次独立入队（先 wait 后 signal）在同一队列 FIFO 执行，`before_write poll=0` ⇒ 第二条命令没有跑 | "先 CQS_WAIT 后 FENCE_SIGNAL"的顺序设计**正确**，可作为 future completion-signal 的骨架 | 同 E3 日志 | 已验证（真机） |
| 2026-10-18 | **sync_file `read()` 在本内核返回 `EINVAL`**：`poll()` 可用（返回 `POLLIN`），但读不到 fence 状态 | 判据只能用 `poll()`，不要写 `read()` 分支 | `kcpu-e1e3-20261018/e2.log`（`E2 read ret=-1 errno=22`） | 已验证（真机） |
| 2026-10-18 | **`kctx->csf.event_pages_head` 在本内核里是空链表**：只有 `INIT_LIST_HEAD`(reg_track.c:979)、遍历(mem_linux.c:148)、以及 teardown 的 `WARN_ON(!list_empty)`，**没有任何插入点** | ⇒ `kbase_find_event_mem_region()` 恒返回 NULL，CQS 地址解析**只能**靠 `kbase_region_tracker_find_region_enclosing_address()` 兜底。SAME_VA 区域挂在 `vma->vm_start`（`kbase_gpu_mmap`，mem_linux.c:2868），所以传 mmap 返回地址是对的 | `mali_kbase_reg_track.c:979`、`mali_kbase_mem_linux.c:140-165,269` | 已验证（源码） |

| 2026-10-18 | ⭐ **KCPU 的 CQS 串行性端到端成立**：队列 A 入队 `CQS_WAIT(addr,0)` + `FENCE_SIGNAL`，队列 B 入队 `CQS_SET(addr)`。`before_set poll=0` → `after_set poll=1 revents=POLLIN`，且内存里 `cell=1`（内核执行了 `evt[VAL/4]++`） | 这走的是和 GPU 完成**同一条**唤醒路径（`kbase_csf_event_signal`→`event_cqs_callback`→`enqueue_kcpuq_work`），所以只剩"GPU 写→门铃"一跳未验 | 真机 `kcpu-e1e3-20261018/e3a.log` + `e3a-retry.log`，两次一致 | 已验证（真机） |
| 2026-10-18 | **CQS_SET 必须来自另一条队列**：把 `CQS_SET` 排在**同一个** `CQS_WAIT` 之后的同队列里，它自己也被挡住，`cell` 保持 0 | 队列 FIFO 且 `nr_commands == 1`，**不能自己解开自己**。需要独立队列（或 GPU 写入） | 首次 E3a 尝试 `cell=0`；改两队列后 `cell=1` | 已验证（真机） |
| 2026-10-18 | ⭐ **删除持有未满足 `CQS_WAIT` 的队列是干净的**：`KCPU_QUEUE_DELETE` 返回 0，dmesg 无 `WARN_ON`、无 kbase/mali 新增 | 实证中止协议 §2 第 1 步：`delete_queue(drain=true)` 会清掉挂起的 wait，**不需要重启设备**。再次印证"`CQS_WAIT` 不阻塞内核线程" | 真机 `kcpu-e1e3-20261018/e4w.log` + 前后 dmesg 对比 | 已验证（真机） |

| 2026-10-18 | ⭐ **E4 通过：GPU 写 → 门铃 → KCPU 完成信号，整条链闭合**。生产 submit 流照常发 `SYNC_ADD64 +1 @ seqno`，另外在独立 KCPU 队列上挂 `CQS_WAIT(seqno_va, target-1)` + `FENCE_SIGNAL`。三次运行一致：`KBASE_KCPU_EVENT_RESULT ready=1 revents=0x1` | 补齐了 E3a 没覆盖的 1–3 步（GPU 写 → 固件抖门铃 → `kbase_csf_event_signal`）。⇒ kbase 上**能**拿到由 GPU 完成驱动的 `sync_file` fd。这是 U2 的目标，也是 U7 timeline 的前提 | 真机 `e4-20261018/e4.log`；载体 `kcpu-probe/e4_icd_probe.c`；对照 `baseline.log`（关标志，不触发） | 已验证（真机，3 次） |
| 2026-10-18 | **探针的 seccomp `denied[]` 里 KCPU 三个 ioctl 是测试断言**（`panvk_kbase_device_probe.inc:522`），把它们移进 `allowed[]` 会让该断言失效 | ⇒ 自测**不能**借道 `panvk_kbase_enum_probe`。真机验证走独立二进制（dlopen ICD，无守卫），与 import-probe 的处理一致。第一次尝试报 `stage=create_failed errno=1(EPERM)` 即为此 | `panvk_kbase_device_probe.inc:512-527`；`e4-20261018/e4.log` 前后两次 | 已验证 |
| 2026-10-18 | **CQS_WAIT 阈值是 `target - 1`**：内核判据 `evt[VAL/4] > val`。本次 `SYNC_ADD64` 把 seqno 从 1 加到 2，`targets[0]=2`，故阈值为 1 | 写成 `target` 会永远不等（等的是下一帧）；写成 0 会在写入前就通过，失去意义 | `mali_kbase_csf_kcpu.c:816`；`e4.log` 中 `expect=1 target=2`，且 `before` 观察为未就绪（E3a 同构） | 已验证 |

| 2026-10-18 | **`CmdClearColorImage` 走完整图形路径**：`panvk_vX_cmd_meta.c:381` 调 `meta_gfx_start`，内部 `vk_meta_clear_color_image` → `vk_meta_clear.c:476-511` `clear_image_level_layers`，直接 `CmdBeginRendering`/`CmdEndRendering`（`loadOp=CLEAR`）⇒ 产生 VERTEX_TILER + FRAGMENT chunk | 所以它**不是**"轻量、不碰 tiler"的路径；但它仍是合适的第一条首帧试验（无应用数据、判据简单、验的是骨架） | `vk_meta_clear.c:476-511`、`panvk_vX_cmd_meta.c:364-385` | 已验证（源码） |
| 2026-10-18 | **`panvk_instr_begin_work` / `end_work_async` 对 `WORK_TYPE_META` 只发 utrace，不发 CS 指令**：`panvk_vX_instr.c:145-195` 的 switch 各分支只调 `trace_begin_*`；`end_work_async` 的 `WORK_TYPE_META` 分支（`:222-224`）只调 `trace_end_meta` | ⇒ `meta_gfx_start/end` 本身**不产生 chunk**。是否进 graphics 路径取决于 meta 内部是否真的 begin/end rendering，别把 utrace 标记误当成提交 | 同左 | 已验证（源码） |
| 2026-10-18 | **首帧的三个提交门控**（`panvk_kbase_compute_queue.inc`）：`:487` 拒绝 `contains_event_waits`/`contains_timestamp_queries`/`uses_poly_heap`；`:502` graphics 需 `graphics_ready`；`:504` graphics 需成功创建 replacement tiler heap | 首帧不能带 event wait / timestamp query / poly heap | 同左 | 已验证（源码） |
| 2026-10-18 | ⚠️ **`mali-re/文档/5-异步改造规格书.md` §4 的"CQS_WAIT 挂死内核 worker"一条与本仓库实测冲突**：源码是 `process_next=false` 返回，真机删除持有未满足 wait 的队列返回 0 且无 WARN | 该条应读作"队列停住"而非"线程挂死"。首帧阶段不用 KCPU，不影响当前计划，但**引用该文档时要留意** | `csf/mali_kbase_csf_kcpu.c:2645`；`kcpu-e1e3-20261018/e4w.log` | 已验证（源码 + 真机） |

| 2026-10-18 | ⭐ **首帧成立：kbase 图形提交渲染出正确像素**。`CmdClearColorImage` 64×64 RGBA8，线性布局，`vkQueueWaitIdle` 后 CPU 映射回读：红/绿两色各 `match=4096/4096`。清屏前把映射区填 `0xCD`，排除"读到常量"；两色不同，排除"读错地址" | 证明 graphics 提交骨架 + 被跳过的 heap 指令形状**不影响渲染正确性**。范围限于：无 draw call、无顶点、无深度、无 WSI、单队列单次 | 真机 `m5b2-prep-20261018/s2.log`，2 次一致；`s2_pixel_probe.c` | 已验证（真机） |
| 2026-10-18 | ⭐ **`kbase_kmod_heap_create` 曾错误要求 heap context 页对齐**，导致**第二个 graphics heap 必然失败** ⇒ 首帧不可能成功。内核从共享 context 池按 `HEAP_CTX_SIZE(32)` 对齐到 cache line（64）步进分配，只有槽位 0 页对齐。实测 heap_ctx `0x6000000000`(第 1 个) → `0x6000000040`(第 2 个) | 已修：只保留 `first_chunk` 的页对齐（真映射）。回归测试 `heap_context=0x90000040` 必须成功；oracle 验证加回旧检查会在 line 864 挂。M5b-1 未暴露因为只做 compute 不建 graphics heap | `kbase_kmod.c:813-826`；`mali_kbase_csf_heap_context_alloc.c:26,53-54,152` | 已验证（源码 + 真机 + 单测） |
| 2026-10-18 | ⭐ **5 处 heap 跳过是必需的，不是优化**：强制 `cmdbuf_skips_gpu_heap_ops()` 返回 false 跑同一条 clear，进程 **SIGSEGV 于提交前**。因为它会走 `setup_tiler_oom_ctx()`，解引用 `dev->tiler_oom.handlers_bo`，而该字段在 kbase 路径**从未初始化**（全仓库仅测试手动设过） | ⇒ 跳过依赖的数据在 kbase 上不存在。若将来要支持 TILER_OOM 处理，必须先建立 `handlers_bo` 分配路径 | `panvk_vX_cmd_draw.c:4534-4545`；实验 `PANVK_KBASE_NO_SKIP=1`（临时，已撤销） | 已验证（真机） |
| 2026-10-18 | **panvk 无 `CmdCopyImageToBuffer`**，像素回读只能靠线性布局 + CPU 映射：`PANVK_DEBUG=linear` 强制 `DRM_FORMAT_MOD_LINEAR`，`vkGetImageSubresourceLayout` 取真实 `rowPitch`（64 宽 RGBA8 实测 256） | 回读探针必须显式挑 `HOST_VISIBLE` 内存类型；index 0 是 `DEVICE_LOCAL` only，不可 map | `panvk_image.c:168-175,952-957`；`panvk_physical_device.c:258-291` | 已验证（源码 + 真机） |

| 2026-10-18 | ⭐ **内核无 dma-buf exporter（硬约束，四路交叉）**：①厂商源码 `dma_buf_export`/`DEFINE_DMA_BUF_EXPORT_INFO`/`dma_buf_ops` 零出现；②uapi 无 export ioctl；③所有 fd 返回路径仅 sync_file(`sync_file.c:87,94`、`kcpu.c:2031,2085`)、kinstr_jm、kinstr_prfcnt、timeline；④`MEM_QUERY` 只支持 `COMMIT_SIZE`/`VA_SIZE`/`FLAGS`（uapi `mali_kbase_ioctl.h:133-135`），无 fd | ⇒ kbase 只能 **import** dma-buf，不能导出。`panvk_wsi_init()` 在 kbase 路径被整体跳过（`panvk_physical_device.c:509-513`）。WSI 只能走 `sw_device` 软件呈现或借道 Android，**不是补个 ioctl 能解决的** | `mt6991-kernel-src/.../mali_kbase_mem_linux.c`(`dma_buf_get`/`map_attachment` only)；全仓库 grep | 已验证（源码） |
| 2026-10-18 | **首帧封板**：`CmdClearColorImage` 64×64 RGBA8 在 kbase 上产生 `graphics_chunk=1 graphics_subqueues=0x3`，提交完成，像素 `match=4096/4096`（红/绿两色，2 次一致）。**范围限于**：无 draw call/顶点/管线/深度/多 pass/其他格式/尺寸/并发/性能，**且未上屏**（无 WSI） | 不得外推为"图形管线通了"。顶点路径完全未验证 | `m5b2-prep-20261018/FIRST-FRAME-SEALED.md` | 已验证（真机），已封板 |

| 2026-10-18 | ⭐ **完整图形管线在 kbase 上跑通**：render pass + framebuffer + graphics pipeline（SPIR-V → Mali 指令）+ `vkCmdDraw(3)`，全视口三角形（`gl_VertexIndex` 生成，无顶点缓冲），清屏红 + 片元绿。回读 `red=0 green=4096 other=0`，`green_box=(0,0)-(63,63)` 覆盖全图，2 次一致 | ⇒ shader 编译、图元装配、光栅化、片元输出全部可用。**比 clear 强得多**（clear 走 meta 固定管线） | 真机 `m5b2-prep-20261018/s4.log`；`first-frame-probe/s4_draw_probe.c` + `shaders/` | 已验证（真机） |
| 2026-10-18 | **kbase 只能用传统 render pass，不能用动态渲染**：`panvk_kbase_physical_device_capabilities`（`panvk_kbase_device.c:88-95`）把 extensions 和 features **整体置零**，覆盖了 `panvk_vX_physical_device.c` 的正常表（`.dynamicRendering = true` 在 `:469`） | ⇒ kbase 上任何依赖扩展或 feature 的路径都不可用。`vk_meta` 的 clear 能调 `CmdBeginRendering` 是因为驱动内部调用不查扩展；应用侧必须走 `VkRenderPass`。实测 render pass 路径返回 0 | `panvk_kbase_device.c:88-95`、`panvk_vX_physical_device.c:44-49,311-317,469` | 已验证（源码 + 真机） |

| 2026-10-18 | ⭐ **纹理最小闭环可用**：单张 8×8 RGBA8 纹理 + 单个 `COMBINED_IMAGE_SAMPLER` 描述符 + nearest 采样，输出 64×64 回读 `match=4096 mismatch=0`。纹理用图案（`R=x*32, G=y*32, B=(x+y)*16`）而非纯色，可区分采错纹素/采错mip/读未初始化/UV反向 | ⇒ 描述符更新、描述符到达 shader、采样指令执行、采样值正确，四件事分别成立。**范围限于**：单 binding、单 set、单 pool、无动态偏移、无数组/多维/cubemap、无 mipmap/线性过滤、仅 RGBA8 | 真机 `s5-texture-20261018/run.log`，2 次一致 | 已验证（真机） |
| 2026-10-18 | ⭐ **纹理路径不依赖 dma-buf exporter**：`pan_kmod_bo_export` 全仓库只有**一个**生产调用点 `panvk_device_memory.c:446`（`vkGetMemoryFdKHR`，外部句柄导出）；图像内存来自 `pan_kmod_bo_alloc`（`panvk_device_memory.c:119`）自己创建；描述符存 GPU VA 不是 fd | ⇒ 只用自建 BO 采样不撞 U16 硬约束。只有"导入外部图像"或"把结果交给别的进程"才会撞上 | `panvk_device_memory.c:119,446`；全仓库 grep | 已验证（源码） |
| 2026-10-18 | ⭐ **VAF 顶点属性取数可用**：`stride=20`、`VEC2`@0、`VEC3`@8，三顶点三色，插值结果 1830 个内部像素全部与重心坐标计算值吻合（误差≤2） | ⇒ 顶点缓冲创建/绑定/上传、按 stride 取数、多属性不同格式、属性插值全部正确。**范围限于**：无索引、无 instancing、单流、仅 float 格式、无 step rate | 真机 `s6-vbuf-20261018/run.log`，2 次一致 | 已验证（真机） |
| 2026-10-18 | **超出视口的顶点会被裁剪，改变边界处的重心坐标**：用 `(-1,-1),(3,-1),(-1,3)` 时，`(32,32)` 处期望/实测量吻合但 `(4,4)` 处差很大，差异集中在被裁剪一侧 | ⇒ 写几何判据时顶点必须在 `[-1,1]` 内，否则期望公式要包含裁剪后的多边形，容易误判为驱动 bug | `s6-vbuf-20261018/RESULT.md` 问题 1 | 已验证（真机 + 分析） |
| 2026-10-18 | **线性图像的行距不一定是 width×4**：同一进程里 8×8 纹理 `row_pitch=64`，64×64 渲染目标 `row_pitch=256` | ⇒ 回读/上传必须用 `vkGetImageSubresourceLayout` 的真实 `rowPitch` 逐行处理，不能按 `width*4` 算 | `s5-texture-20261018/run.log` 的 `ROW_PITCH` | 已验证（真机） |

| 2026-10-18 | ⭐ **索引绘制可用**：4 角点 + 6 索引画四边形，`vkCmdDrawIndexed`，回读 `match=4096 mismatch=0 uncovered=0`。判据是几何性的：索引被忽略退化成画前 3 顶点会留 1024 个未覆盖像素 | ⇒ 索引缓冲取数、`vkCmdDrawIndexed`、三角形装配正确 | 真机 `s7-s9-20261018/s7_indexed.log` | 已验证（真机） |
| 2026-10-18 | ⭐ **深度测试可用，且已用顺序对照证伪"后画的赢"**：两个完全重叠的三角形（近 z=0.25 红 / 远 z=0.75 绿），`VK_COMPARE_OP_LESS` + 深度写入。near-first 与 far-first **结果相同**（`red=1024 green=0 blue=3072`），深度缓冲回读 `0x3e800000`(=0.25f) | 单跑一种顺序无法区分"深度生效"与"后画的赢"，必须两种都跑。⇒ 深度测试、深度写入、深度回读、顺序无关性全部成立 | 真机 `s7-s9-20261018/s8_depth.log`；两种顺序 | 已验证（真机） |
| 2026-10-18 | ⭐ **索引+纹理+深度组合可用**：中央 32×32 采样 8×8 纹理逐像素正确（`match=1024`），外围保持清屏蓝（`ok=3072`），深度全程 0.25（`valid=4096 far=0`）。两种绘制顺序结果相同 | ⇒ 三套机制同时工作互不干扰。**范围**：单 render pass、单绑定、单采样器、无 mipmap/MSAA | 真机 `s7-s9-20261018/s9_combo.log` | 已验证（真机） |
| 2026-10-18 | **写深度/索引/纹理实验时的三个测试陷阱（都不是驱动问题）**：①两组三角形若 z 由位置推导且位置相同，则 z 相同，深度无可比差异——z 必须是独立顶点属性；②清屏色若与三角形同色，无法区分"画了"与"没画"；③深度统计若只数 `0<dz<1`，会漏掉未覆盖区域，得 1024 而非 4096 | 这三个都会表现为"驱动坏了"，实际是判据错。写几何判据时应逐条检查 | `s7-s9-20261018/RESULT.md` 各节 | 已验证（真机 + 分析） |
| 2026-10-18 | **panvk 描述符实现完整，kbase 的缺口在能力声明而不在实现**：8 种描述符类型、数组（`descriptor_set.c:674,682,695,704` 逐元素）、动态偏移（`cmd_desc_state.c:53`）、可变描述符、内联 uniform、采样器全状态（`sampler.c:111-154`）都有代码 | 但 `panvk_kbase_physical_device_capabilities`（`kbase_device.c:88-95`）把 features 置零 ⇒ 应用打不开。⇒ 多数缺口的工作量在"验证 + 放开声明"，不在写实现 | `s7-s9-20261018/DESCRIPTOR-GAPS.md` | 已验证（源码） |

| 2026-10-18 | ⭐ **samplerAnisotropy 在 kbase 上实际生效**：64×64 竖条纹纹理（1 纹素宽）+ 7 级 mip 链，四边形纵向压缩 8:1。iso 采样输出 `range=[127,127]`（每像素精确等于条纹平均值，证明采样与 mip 都发生了），aniso(`maxAnisotropy=16`) 输出 `range=[0,255]`（条纹保留）。2 次一致 | ⇒ 驱动实现有、但 `FEATURES samplerAnisotropy=0` 声明无。**声明与实现不一致，可放开但需单独一步** | 真机 `s10-aniso-20261018/run-validation.log` | 已验证（真机，2 次） |
| 2026-10-18 | ⭐ **此前所有探针的 Validation Layer 从未生效**：探针用 `dlopen(icd.so)` + `vk_icdGetInstanceProcAddr` **直连 ICD，绕过 loader**，layer 无法介入。改为 `dlopen("libvulkan.so.1")` + `vkGetInstanceProcAddr` + `VK_ICD_FILENAMES` 后 layer 正常工作 | 后续探针必须走 loader 模式（`S10_USE_LOADER=1` 同款），否则"开 Validation"是形式上的 | `s10_aniso_probe.c`；`MODE loader` 行 | 已验证（真机） |
| 2026-10-18 | ⚠️ **所有回读探针对 `TILING_OPTIMAL` 图像调用 `vkGetImageSubresourceLayout`，违反 VUID-vkGetImageSubresourceLayout-image-07790**。能工作是因为 `PANVK_DEBUG=linear` 让 panvk 实际按线性布局分配，但规范上属未定义行为 | 不影响已有结论（上传/回读用同一布局，自洽），但**后续探针应创建 `VK_IMAGE_TILING_LINEAR`**。换驱动/配置会失效 | Validation 报 8 次同 VUID | 已验证（Validation Layer） |
| 2026-10-18 | **kbase 内核不提供 Vulkan feature 答案**：`mali_base_hwconfig_features.h` 是内核内部硬件特性（FLUSH_REDUCTION 等），与 `VkPhysicalDeviceFeatures` 不同层次；`mali_kbase_gpuprops.c:708-716` 注释明确 `core_features` 寄存器"含义随 GPU 而异"，CSF GPU 上只解 `num_exec_engines`（且在 `!MALI_USE_CSF` 分支）；全内核 grep `anisotrop`/`DEPTH_CLAMP` 零命中 | ⇒ `depthClamp`/`samplerAnisotropy` 这类 Vulkan feature **只能靠真机行为裁决**，内核源码里没有答案。别再去内核里找 | `mt6991-kernel-src/mali-r49p1` 全树 | 已验证（源码） |

| 2026-10-18 | ⭐ **samplerAnisotropy 已放开声明**：`panvk_kbase_physical_device_capabilities` 现在返回 `.samplerAnisotropy = true`，其余仍为零。复验：`FEATURES samplerAnisotropy=1`、`range iso=[127,127] aniso=[0,255]`、**Validation 零报错**、131/131 单测通过 | 连带更新两处"features 全零"断言：`panvk_kbase_device_test.c:204`（改为只允许 samplerAnisotropy）与 `panvk_kbase_enum_probe.c`（`optional -= 1` 并显式断言） | `panvk_kbase_device.c:87-100`；真机 `probe-fix-and-aniso-declare-20261018/s10-final.log` | 已验证（真机 + 单测） |
| 2026-10-18 | ⚠️ **`/tmp/kbase-icd/` 副本过期陷阱**：复验时 `FEATURES` 仍为 0，因为 `/tmp` 里是 17:52 的旧 ICD 而构建产物是 20:14。**每次重新构建后必须重新拷贝** | 这是把 ICD 复制到 `/tmp` 规避 `/home/zl` 权限问题的副作用 | 时间戳对比 | 已验证（真机） |
| 2026-10-18 | **`VUID-VkSamplerCreateInfo-anisotropyEnable-01070` 的两层含义**：报 "feature was not enabled" 指 `VkDeviceCreateInfo.pEnabledFeatures` 没开，不是设备不声明。探针创建设备时未传 `pEnabledFeatures` 也会触发 | 区分方法：先看 `vkGetPhysicalDeviceFeatures` 是否返回 1，再看创建时是否启用。两者都满足才不报 | `s10_aniso_probe.c` 的 `pEnabledFeatures` | 已验证（Validation Layer） |
| 2026-10-18 | **S2 的 clear 曾违反两条 VUID**：`vkCmdClearColorImage` 要求 `TRANSFER_DST` usage 且 layout 只能是 `TRANSFER_DST_OPTIMAL`/`GENERAL`/`SHARED_PRESENT_KHR`。早期为迁就 `vk_meta` render pass 改成了 `COLOR_ATTACHMENT_OPTIMAL`，属误用（meta 内部自己做 layout 转换） | 已修，`match=4096/4096` 不变，VUID 消失 | `s2_pixel_probe.c`；Validation 日志 | 已验证（Validation Layer） |

| 2026-10-18 | ⭐ **depthClamp 可用**：四边形置于远平面外（z=2.0），`depthClampEnable=FALSE` 时 `covered=0`（正确裁剪），`TRUE` 时 `covered=4096` 且深度回读 1.000。两条管线只差这一个字段 | ⇒ 钳制行为真实存在。**常见误读**：`covered=0` 不是"没生效"而是正确的裁剪 | 真机 `s11-s13-and-vkmark-20261018/final-s11_depthclamp.log` | 已验证（真机） |
| 2026-10-18 | ⭐ **depthBiasClamp 可用**：同几何同 bias（`constant=8e6, slope=0`），`clamp=0` 时深度 `0.488419`，`clamp=0.05` 时 `0.300000`。`0.25+0.05=0.30` 精确对应 clamp 参数 | 判据必须读**深度缓冲**，不能看颜色——未钳制的 bias 只推到 0.488，未越过远三角形(0.75)，颜色不变 | 真机 `final-s12_depthbias.log` | 已验证（真机） |
| 2026-10-18 | **depth bias 的量级陷阱**：D32F 的 `r ≈ 2^-23`，`constantFactor=1.0` 只产生约 `1.2e-7` 的深度位移，肉眼与判据都不可见。要产生 ~0.24 的位移需 `constant ≈ 8e6` | 写 bias 探针时先估算 `r`，别用"看起来合理"的小数值，否则会误判成"bias 没生效" | `s12_depthbias_probe.c` 注释 | 已验证（真机） |
| 2026-10-18 | ⭐ **independentBlend 可用**：MRT 两附件，片元都输出 `(1,0,0,1)`，附件 0 加法(`ONE/ONE`)、附件 1 replace。回读 `attach0=255,64,64`（0.25×255=64，精确等于清屏+源）、`attach1=255,0,0`（精确等于源） | 两者不同且各自符合预期 ⇒ 不是"用同一套状态"。颜色精确匹配是关键证据 | 真机 `final-s13_independentblend.log` | 已验证（真机） |
| 2026-10-18 | ⭐ **四项 feature 已放开声明**：`samplerAnisotropy` / `depthClamp` / `depthBiasClamp` / `independentBlend`，每项都有对应真机探针，注释里写证据来源。`optional -= 4`，131/131 单测通过 | 验证与声明有循环依赖：未声明时 `vkCreateDevice` 报 `VK_ERROR_FEATURE_NOT_PRESENT`，探针建不起设备。解法是临时 env 门控，验证后移除 | `panvk_kbase_device.c:87-115` | 已验证（真机 + 单测） |
| 2026-10-18 | **vkmark 没有 offscreen 模式，必须走 WSI**：`--winsys [xcb,wayland,kms]`，`--present-mode`，`strings` 扫描无 headless/offscreen。而 `panvk_wsi_init()` 在 kbase 上被整体跳过，且内核无 dma-buf exporter | ⇒ **vkmark 的阻塞点是 WSI，不是 feature 声明**。继续放开 feature 不会让它可运行。apiVersion 1.0 在 vkmark 语境下不是首要阻塞 | `s11-s13-and-vkmark-20261018/VKMARK-ASSESSMENT.md` | 已验证（静态） |

| 2026-10-18 | ⭐ **Mesa WSI 的 SHM 回退已实现，开关是 `sw_device`**：`wsi_common.c:89` `wsi->sw = device_options->sw_device`。开启后 `:221` **跳过 `GetMemoryFdKHR`**（不需要 dma-buf 导出）、`:3086` 选 `HOST_CACHED` 内存、`wsi_common_x11.c:246` 走 MIT-SHM | ⇒ **不需要新写 SHM 路径**，只需 1 行开关 + 放开 surface 扩展。这绕过了 U16 的 dma-buf 硬约束 | `src/vulkan/wsi/wsi_common.c`、`wsi_common_x11.c` | 已验证（源码） |
| 2026-10-18 | ⭐ **`sw_device = true` 时 X11 WSI 不检查 DRI3**：`wsi_common_x11.c:646` `if (!wsi_device->sw) { if (!wsi_x11_check_for_dri3(...)) return false; }` | ⇒ 没有 DRI3 的 X server（如 Xvfb）可以工作。这是路线 A 可行的关键前提 | 同左 | 已验证（源码） |
| 2026-10-18 | **环境实测**：`Xvfb` 已装（2:21.1.16）、启动 `:98` 后 23 个扩展**含 MIT-SHM**、**不含 DRI3**；`weston`/`Xwayland` 也已装；`libxcb1-dev`/`libx11-xcb-dev` 已装 | Xvfb 缺 DRI3 对 `sw_device` 路径不成问题。路线 A 的环境障碍比预想小 | `xdpyinfo` 实测 | 已验证（环境） |
| 2026-10-18 | **无 render node、无 vgem**：`/dev/dri/card0` 是 `mediatek-drm`（显示控制器）不是 Mali，`/dev/dri/renderD*` 不存在，`vgem` 未加载（`modinfo` 无输出） | ⇒ DRI3/辅助 render node 路线**不可行**。且即使可行，Mali 结果仍要过 CPU 才能进辅助 buffer，无性能优势 | 设备节点与 `lsmod` | 已验证（环境） |
| 2026-10-18 | **当前构建未启用 X11 平台**：`platforms = []`，`wsi_common_x11.c` 未编译 | ⇒ 走路线 A 需要重配 meson 加 `-Dplatforms=x11`，不只是改代码 | `meson-info/intro-buildoptions.json` | 已验证（构建配置） |

| 2026-10-19 | ⭐ **wait 循环里的 `KBASE_IOCTL_MEM_SYNC` 会饿死 CSG 调度**：原 `kbase_csf_wait()` 每 1 ms 对 seqno BO 做一次 `FLUSH_AND_INVALIDATE`，即每秒上千次发在**同一个 `/dev/mali0` fd** 上；该 fd 同时是内核调度 queue group 的通道。表现为 fragment 子队列的 `CS_EXTRACT` 停在最后一条 ring entry 前 80 字节（正好是 `WAIT 0xffff` + `SYNC_ADD64` 的尾部），`CS_ACTIVE=0`，seqno 恰为 target-1，err=0，任何时长（含 30 s）都不恢复 | 改为用户态 `dc civac` + `dsb sy`（与参考实现一致，无 ioctl）后：clear 0/12 → 11/12，vertex 约 4/12 → 9/12 且 370+ FPS | `src/panfrost/lib/kmod/kbase_csf.c` `cache_invalidate_range()`；`/tmp/ci*.log`、`/tmp/lx*.log` | 已验证（真机） |
| 2026-10-19 | ⭐ **kbase 上跨子队列同步必须用 SYSTEM 作用域**：我们给每个子队列建独立的 queue group（3 个 CSG），`MALI_CS_SYNC_SCOPE_CSG` 的信号 fragment 收不到，VT→frag 的依赖永远不满足 | 对应 G925 补丁 077。`panvk_instr.h` 新增 `panvk_cs_signal_scope()`，在 `PANVK_INSTR_SYNC_OPS` 宏内把 CSG 重映射为 SYSTEM，并改掉 `cmd_draw/cmd_event/cmd_query/cmd_buffer` 里的直接 `cs_sync32_*` 调用点 | `src/panfrost/vulkan/csf/panvk_instr.h`、`panvk_vX_cmd_{draw,event,query,buffer}.c` | 已验证（真机） |
| 2026-10-19 | **每次图形提交都换 tiler heap 代价极高**：原实现对每个 graphics submit 做 `kbase_graphics_heap_create` + rearm（两次阻塞提交）+ 销毁旧 heap，等价于每帧多一次 GPU 往返 | 改为每 128 次图形提交换一次（参考实现 `KBASE_TILER_HEAP_RENEW_INTERVAL`）后，512x512 vertex 从 116 FPS 升到 355–393 FPS | `csf/panvk_kbase_compute_queue.inc` `renew_heap`；`/tmp/vx*.log` | 已验证（真机） |
| 2026-10-19 | ⭐ **`wsi_common_x11.c` 的 present 超时判断是错的**：`u_cnd_monotonic_timedwait()` 只返回 `thrd_timedout`，从不返回 `ETIMEDOUT`（`src/util/cnd_monotonic.c:137`），所以超时分支永不命中，落到 `VK_ERROR_DEVICE_LOST` | 对应 G925 补丁 084。已改为 `thrd_timedout` | `src/vulkan/wsi/wsi_common_x11.c:3075` | 已验证（源码） |
| 2026-10-19 | ⚠️ **不能在 vkmark 运行期间覆盖 `/tmp/kbase-icd/libvulkan_panfrost.so`**：`cp` 会截断正在被 mmap 的文件，进程收到 SIGBUS，看起来像驱动崩溃 | 部署 ICD 前先确认没有测试在跑 | `/tmp/lv11.log`（Bus error） | 已验证（真机） |
| 2026-10-19 | ⭐ **kbase 异步提交已接通，用 `PANVK_KBASE_ASYNC_SUBMIT=1` 开启**：`kbase_compute_submit` 发布后立即返回，signal 用 `panvk_kbase_sync_set_pending(&kbase_completion_ops, queue, targets)` 挂成 completion。`ops.wait` 必须把"时间片到期"翻译成 `VK_TIMEOUT`，否则 sync 层的 5ms 分片会被当成设备丢失（首版 10/10 全挂就是这个） | 实测 512x512 vertex：同步 317 FPS / 异步 253 FPS，两者 10/10 通过。**异步更慢**，因为 SHM 软件呈现本来就要等这一帧；而且残余卡死会从"5 秒后 DEVICE_LOST"变成"fence 永不就绪"（无限等待）。所以默认仍关 | `csf/panvk_kbase_compute_queue.inc` `kbase_completion_wait`；`/tmp/as*.log`、`/tmp/sy2_*.log` | 已验证（真机） |
| 2026-10-19 | **ring 满时必须等待再重试**：异步模式下 CPU 会跑到 GPU 前面，64 KiB 的 userspace ring 会填满，`queue_prepare_locked` 返回 `EAGAIN`。已加"等当前 published target 再重试"的循环 | 否则表现为 "kbase submission failed" | 同左 | 已验证（源码 + 真机） |
| 2026-10-19 | **单测需同步更新**：`panvk_kbase_compute_queue_test` 用 `PANVK_KBASE_TEST_HEAP_INTERVAL=1` 保持覆盖 heap 更新路径；`panvk_kbase_cmd_draw_test` 的 `check_sync_add` 现在按 kbase/非 kbase 断言 CSG 或 SYSTEM 作用域 | 86/86 通过 | `tests/panvk_kbase_{compute_queue,cmd_draw}_test.c` | 已验证（单测） |


## 更正记录（续）

| 日期 | 原条目 | 更正为 | 依据 |
|---|---|---|---|
| 2026-10-19 | 2026-10-18「⭐ U1 源码裁决：kbase 的 started-only heap 统计是安全的」条目结尾称：**「kbase 跳过 `cs_set_exception_handler(TILER_OOM)` ⇒ 校验根本不会执行」** | **该结论不成立。** 校验在 `handle_oom_event()` 内，只要固件发出 `CS_REQ.TILER_OOM` 就会执行；`group->csi_handlers & BASE_CSF_TILER_OOM_EXCEPTION_FLAG` 只决定「增量渲染」那一个分支（`pending_frag_count == 0 && (err == -ENOMEM \|\| err == -EBUSY)`），**不影响**前面的堆统计校验，也不影响 `kbase_csf_tiler_heap_alloc_new_chunk()` 与 `validate_allocation_request()`。跳过用户态 TILER_OOM handler 只是让内核走普通 chunk-grow 路径 | `mt6991-kernel-src/.../csf/mali_kbase_csf.c:2075-2096`（统计校验与 alloc 调用都在 `csi_handlers` 判断之前）、`:2089-2096`（csi_handlers 分支）、`csf/mali_kbase_csf_tiler_heap.c:905-937`（`validate_allocation_request`） | 已验证（官方 GPL 源码，2026-10-19 复核） |
| 2026-10-19 | ⚠️ **syncobjs 初始 seqno 1 → 16 的"余量"实验**（`csf/panvk_kbase_compute_queue.inc` `kbase_compute_init` **仅此一处**，wait 条件未动）：fragment 的 `wait_finish_tiling` 用 `GREATER`，设计上只靠 init 的 **1 个单位**余量（`cell0 = 1 + 已执行 flush 数` vs `target = 申报 flush 数`）。改为 16 后，vertex 512×512 `-d 2` 连跑 **10/10 通过**，FPS 282–340；此前同条件约 10–15% 偶发 `VK_ERROR_DEVICE_LOST` | **只证明"余量掩盖了问题"，不证明根因已定位。** 副作用：前 15 次 tiler flush 之前，fragment 的等待会无条件成立（`cell0=16 > target`），即早期帧**不再等待 VT**；vkmark 下未观察到故障，但这是真实风险，不能当正式修法 | `/tmp/buf10_1..10.log`；对照 `/tmp/v2_3.log`（`cell0=419` vs `frag_target=844`，说明 cell 计的是 **tiler flush 次数**，不是 ring 提交次数，两者约 2:1） | 已验证（真机 10 次）；根因**未定位** |

## 调试发现（续 2026-10-19：kick 与 sync 重估）

| 日期 | 发现 | 含义 | 证据 | 状态 |
|---|---|---|---|---|
| 2026-10-19 | ⭐ **`KBASE_IOCTL_CS_QUEUE_KICK` 在"组挂起于 sync wait"时是纯空操作**：`kbase_csf_queue_kick` 只把 queue 挂到 `pending_gpuq_kick_queues`；调度线程的 `kbase_csf_process_queue_kick` → `kbase_csf_scheduler_queue_start`，其中 `group->run_state == SUSPENDED_ON_WAIT_SYNC && CS_STATUS_WAIT_SYNC_WAIT_GET(queue->status_wait)` 分支只打一句 `dev_dbg("blocked queue ... was kicked")` 就返回 0，**不调度、不写任何 doorbell**。只有非该分支且组在 slot 且 `cs_enabled` 时才 `kbase_csf_ring_cs_user_doorbell`（写该 queue 的 `DOORBELL` 寄存器）；组不在 slot 则仅置为 runnable，等下一次 tick 重新编程 CSG | 每个子队列一个 CSG + 单 queue 的布局下，fragment 阻塞时被踢的正是这个 queue，`status_wait` 非零 ⇒ 500 ms 一次的 rekick 全部被吞掉，30 s 无效可完全由这条解释。`CS_QUEUE_KICK` 与 `CS_DOORBELL` **不等价**（至少在这个关键状态） | `mt6991-kernel-src/mali-r49p1/.../csf/mali_kbase_csf.c:945-992, 3886-3930`；`csf/mali_kbase_csf_scheduler.c:2093-2122` | 已验证（GPL 源码） |
| 2026-10-19 | ⭐ **kbase 自己写明了 sync wait 的重估触发条件是"任意 CS_DOORBELL/GLB_DOORBELL 写入"**：`kbase_csf_event_signal` 注释 "Write to GLB_DOORBELL would suffice as spec says that all pending synch object wait operations are re-evaluated on a write to any CS_DOORBELL/GLB_DOORBELL register"；`KBASE_IOCTL_CS_EVENT_SIGNAL` → `kbasep_cs_event_signal` → `kbase_csf_event_signal_notify_gpu` → 写 `CSF_KERNEL_DOORBELL_NR` **并且**跑 event callbacks → `check_group_sync_update_worker`（对 `idle_wait_groups` 逐组 `evaluate_sync_update`，满足则移回 runnable） | 这条 ioctl 是内核提供的"请重估所有 sync wait"的用户态杠杆，我们的 rekick 没有用它。对**离槽**的等待组，唯一能把组移回 runnable 的是 host 侧 sync-update worker，其触发源只有 CQS_SET / CS_EVENT_SIGNAL / CSG_REQ_SYNC_UPDATE 回调（固件在组离槽后无法发这个请求） | `csf/mali_kbase_csf_event.c:93-150`；`mali_kbase_core_linux.c:1455-1458`；`csf/mali_kbase_csf_scheduler.c:7033-7113` | 已验证（GPL 源码） |
| 2026-10-19 | **kbase 的 CSG 中断处理末尾不写 doorbell，panthor 每次都写**：`process_csg_interrupts` 以 `process_cs_interrupts` 结束，无 doorbell；kbase 只在异常路径（CS fault 2783/2910、tiler OOM 2234）补 ring。panthor `sched_process_csg_irq_locked` 末尾无条件 `panthor_fw_ring_csg_doorbells(ptdev, BIT(csg_id))`，且 `CSG_SYNC_UPDATE` 还会 queue `sync_upd_work` | panthor 的"每次 CSG 事件后补门铃"在 kbase 不存在 ⇒ 之前"假设成立一半"更新为：缺门铃成立，但 kbase 的设计替代物是 host 侧 sync-update worker（需用户态触发），不是 CSG IRQ 里补 ring | `mt6991-kernel-src/.../csf/mali_kbase_csf.c:3198-3250`；`/home/zl/github/mali-re/外部参考/panthor主线源码/panthor_sched.c:1815-1839` | 已验证（源码对照） |
| 2026-10-19 | **VT 的 SYNC_ADD64 对 fragment 的可见性不构成 coherency 问题**：写侧 `panvk_cs_signal_scope()` 在 kbase 上返回 SYSTEM，`PANVK_INSTR_SYNC_OPS` 把 CSG 重映射为 SYSTEM；实测发射字低字节 `0x01`（scope 位 [2:1]=0）。读侧 `cs_sync64_wait` **没有 scope 操作数**，不存在"CSG 域读"与"System 域写"的矛盾。两侧用同一个 `ctx.syncobjs` 设备地址（同一 `panvk_cs_subqueue_context`、同一 kctx/AS） | GPU→GPU、同一 L2、System 域写 + 无域限定的等待；另有 `panvk_priv_mem_write` 对 `WB_MMAP` 池在写完即 flush（init 的 seqno 对 GPU 可见）。SYNCOBJS 诊断从 CPU 侧读回也看到了 GPU 递增后的值 ⇒ 可见性正常，剩余偏差是计数/申报问题，不是缓存问题 | `src/panfrost/vulkan/csf/panvk_instr.h:94-150`；`panvk_vX_cmd_draw.c:4270-4285`；`panvk_kbase_compute_queue.inc:457-465`；`panvk_mempool.h:180-192`；`panvk_vX_device.c:76-90` | 已验证（源码 + 真机诊断） |

### 候选 B 静态核查（2026-10-19）：panvk 从不触发 host 侧 sync-update

| 日期 | 发现 | 含义 | 证据 | 状态 |
|---|---|---|---|---|
| 2026-10-19 | ⭐ **panvk 生产路径没有任何 `kbase_csf_event_signal_notify_gpu` 触发**：`src/panfrost/lib/kmod/kbase_csf.c` 使用的 ioctl 全集为 REGISTER/BIND/GROUP_CREATE/KICK/TERMINATE/GET_GLB_IFACE/MEM_SYNC/KCPU_*，**没有 `KBASE_IOCTL_CS_EVENT_SIGNAL`**（仅 `tests/panvk_kbase_device_probe.inc:523` 枚举过）；没有 CQS_SET；KCPU 只定义 `FENCE_SIGNAL`/`CQS_WAIT`，且生产唯一调用点 `panvk_kbase_compute_queue.inc:111-120` 被 `PANVK_DEBUG(KCPU_EVENT)` 门控、只对 compute 子队列。内核侧 notify_gpu 只有两处：`CS_EVENT_SIGNAL` handler（`mali_kbase_core_linux.c:1455-1458`）与 CQS_SET/CQS_SET_OPERATION（`csf/mali_kbase_csf_kcpu.c:983,1304`）；`FENCE_SIGNAL` 只 `dma_fence_signal`，不通知 GPU（`:1906-1975`） | ⇒ fragment 组挂起后没有任何 host 事件把它移回 runnable。**候选 B 成立且独立于"计数差 1"**：即使 add/target 完全对齐，只要 VT 的 add 在 fragment 组离槽之后落地，就无人重估 | `src/panfrost/lib/kmod/kbase_csf.c`（ioctl 全集）、`panvk_kbase_compute_queue.inc:96-123`；GPL 源码如上 | 已验证（源码） |
| 2026-10-19 | **`flush_tiling` / `issue_fragment_jobs` 全程只发 CS 指令，无 host 调用**：前者发 `cs_finish_tiling` + `PANVK_INSTR_SYNC_ADD64` + 递增 `relative_sync_point`；后者只做 frag 上下文/FBD/tiler-OOM 的 CS 发射。两者都在 build 阶段，提交仍在 `kbase_compute_execute`，且提交后到 `kbase_csf_wait` 之间没有任何 sync-update 触发 | ⇒ B 的修复点不在绘制路径，只能在提交/等待路径补一次内核级信号 | `panvk_vX_cmd_draw.c:4213-4250, 4487-4545` | 已验证（源码） |
| 2026-10-19 | **最小修复方向（未实施）**：在 `kbase_csf_wait()` 的周期分支（`kbase_csf.c:1095-1109`，当前每 500 ms 对所有未达 target 的队列发 `KBASE_IOCTL_CS_QUEUE_KICK`）改为**每周期发一次 `KBASE_IOCTL_CS_EVENT_SIGNAL`**（`_IO(KBASE_IOCTL_TYPE, 44)`，无载荷），kick 可保留作为调度兜底。该 ioctl → `kbasep_cs_event_signal` → `kbase_csf_event_signal_notify_gpu` → ①GPU 上电时写 `CSF_KERNEL_DOORBELL_NR`（固件重估驻留 CSG 的 wait）；②callbacks → `check_group_sync_update_worker` → 对 `idle_wait_groups` 逐组 `evaluate_sync_update`，满足则 `remove_group_from_idle_wait` + 移回 runnable。触发不必每帧：与现有 500 ms 周期同频即可，避免在 VT add 落地前过早求值 | 副作用：`kbase_csf_event_signal` 会 `atomic_set(event_count)` + `kbase_event_wakeup`，我们的 fd 不 poll 它，无实际影响；GPU 睡眠时只跑 host 侧求值，仍有效 | `mali_kbase_core_linux.c:1455`；`mali_kbase_csf_event.c:93-150`；`mali_kbase_csf_scheduler.c:7048-7113` | 待验证（设计） |

### 候选 B 修复验证（2026-10-19）

| 日期 | 发现 | 含义 | 证据 | 状态 |
|---|---|---|---|---|
| 2026-10-19 | **已实现**：kmod 层新增 `kbase_csf_event_signal()`（`_IO(KBASE_IOCTL_TYPE,44)`），在 `kbase_csf_wait()` 的 500 ms 周期分支里**每周期发一次**，`CS_QUEUE_KICK` 循环保留作调度兜底 | 该 ioctl 同时做两件事：GPU 上电时写 GLB doorbell；触发 host 侧 `check_group_sync_update_worker` 对 `idle_wait_groups` 重估。每 5 s 等待窗口最多 10 次，开销可忽略 | `src/panfrost/lib/kmod/kbase_csf.c`（wrapper + rekick 分支）、`kbase_csf.h` | 已验证（构建） |
| 2026-10-19 | **第一轮（init 16 + signal）**：vertex 512×512 `-p fifo` duration 2，**10/10 通过**，FPS 309–322 | 与 init 16 加 signal 之前的 10/10 相同，无回退 | `/tmp/sig_r1_1..10.log` | 已验证（真机 10 次） |
| 2026-10-19 | ⭐ **第二轮（init 1 + signal）**：同条件 **6/10 通过，4 次 `VK_ERROR_DEVICE_LOST`**（`kbase completion failed`，vkmark `waitIdle` abort）。按日志 mtime 推算：失败 run 耗时 6–7 s、通过 run 2–3 s ⇒ 失败走的是 5 s 等待超时路径，**不是 signal ioctl 立即报错** | ⇒ **A 与 B 叠加，B 单独不是根因**。CS_EVENT_SIGNAL 不能消除 init 1 下的等待失败；init 16 仍需作为独立缓解保留（已恢复源码为 16 并重新部署） | `/tmp/sig_r2_1..10.log`；`stat -c %Y` 时间差 | 已验证（真机 10 次） |
| 2026-10-19 | **proves**：在 512×512/vertex/fifo/duration2 条件下，补 CS_EVENT_SIGNAL 后 init 1 仍有 4/10 失败。**does_NOT_prove**：①根因已完全定位（未消除）；②signal 使失败频率如何变化——没有同条件"init 1 且无 signal"的对照（历史数据用的是 256×256/clear 等不同条件），不能声称频率变好或变差；③失败仍属同一 C3 机制（超时路径一致，但未再抓 groups/SYNCOBJS 现场） | 结论边界必须随后续实验更新 | 同上 | 已验证（实验 + 推理） |

### 状态封存：init 16 + CS_EVENT_SIGNAL（2026-10-19）

| 项 | 内容 | 依据 |
|---|---|---|
| 当前部署 | `syncobjs` init seqno = **16**；`kbase_csf_wait()` 周期分支每 500 ms 发一次 `KBASE_IOCTL_CS_EVENT_SIGNAL`，`CS_QUEUE_KICK` 保留。源码与 `/tmp/kbase-icd/libvulkan_panfrost.so` 一致（`cmp` 通过） | `panvk_kbase_compute_queue.inc:463`、`kbase_csf.c:1102`；`cmp` |
| 状态标注 | **工程缓解，根因未完全定位。** A（计数/申报差，init 余量可掩盖）与 B（离槽等待组缺 host 重估触发）叠加；signal 在理论上正确（GLB doorbell + host sync-update worker），但**当前条件下无独立可观测贡献** | 两轮结果（init16 10/10；init1 6/10，失败为 5 s 超时路径） |
| 已知副作用 | init 16 使**前 15 次 tiler flush 之前 fragment 不再等待 VT**（早期帧同步弱化）。vkmark 下未观察到故障，但这是真实风险 | 缓冲实验 |
| 对照缺口 | 缺“init 1 且无 signal”的同条件对照。因此只能断言：**在 init 16 下 signal 不改变结果**；不能断言 signal 无效 | 本轮未运行该对照 |
| proves | ①init 16 下两轮均为 10/10（含 signal）；②init 1 下补 signal 仍 4/10 失败，失败走 5 s 等待超时路径 | `/tmp/sig_r1_*`、`/tmp/sig_r2_*`、mtime 差 |
| does_NOT_prove | ①根因已完全定位；②signal 对失败频率的作用（无同条件对照）；③失败仍严格为 C3 同一现场（未重抓 groups/SYNCOBJS）；④前 15 帧同步正确性 | 同上 |
| 复追条件 | 仅在 **init 16 下再次复现卡死/DEVICE_LOST** 时重新追根因；否则不再投入 | 决策规则 |
| 边界 | 未碰 WSI/U7/级 C；未动 `panvk_kbase_sync_type` 与 5 处 heap 跳过；异步仍只在 `PANVK_KBASE_ASYNC_SUBMIT=1` 后 | — |

## M1 多场景稳定性（2026-10-19）

| 场景 | 次数 | 通过 | 失败 | FPS 范围 | 失败形态 |
|---|---|---|---|---|---|
| vertex 512×512, fifo, duration 2 | 10 | 10 | 0 | 299–354 | — |
| clear 512×512, fifo, duration 2 | 10 | 10 | 0 | 312–331 | — |
| texture 512×512, fifo, duration 2 | 10 | 10 | 0 | 317–436 | — |

三种 workload 合计 30/30，无新失败形态。命令统一：`vkmark --winsys xcb -s 512x512 -p fifo -b <scene>:duration=2`，环境同前（`PANVK_KBASE_WSI_SW=1`、`PANVK_KBASE_DEVICE=/dev/mali0`、`PAN_KMOD_RESTRICT_TO_BACKENDS=kbase`、`DISABLE_WSI_LAYER=1`）。日志 `/tmp/m1_<scene>_1..10.log`。

## 当前能力清单（截至 2026-10-19，仅列真机验证过的内容）

| 类别 | 已验证（真机） | 未验证 / 不承诺 |
|---|---|---|
| vkmark | vertex/clear/texture 各 10/10；此前 7 个场景均可跑，300–325 FPS | 长时间稳定性（>2 s/次）、并发多进程、suspend/resume |
| 像素/绘制 | `CmdClearColorImage` 像素精确；完整图形管线三角形；VAF（stride/格式/插值）；索引绘制；深度测试/写/回读且与绘制顺序无关；索引+纹理+深度组合 | MSAA、多 render pass、secondary command buffer、动态渲染（扩展未声明） |
| 纹理 | 单 binding 的 8×8 RGBA8 + COMBINED_IMAGE_SAMPLER；nearest/linear；aniso（含 mip 链）；线性布局 rowPitch | cubemap/数组/多采样器/多 set、动态偏移、压缩格式、其他格式 |
| Feature 声明 | samplerAnisotropy、depthClamp、depthBiasClamp、independentBlend（各有真机探针） | 其余 feature/extension 声明为零；apiVersion 仅 1.0 |
| 同步/调度 | 跨子队列 SYSTEM scope；tiler heap 128 次续期；无 ioctl 风暴的等待；init16 + CS_EVENT_SIGNAL 工程缓解；异步提交（开关后） | A+B 根因未定位；前 15 次 flush 的早期帧不等 VT；异步默认关且更慢 |
| WSI | X11 MIT-SHM 软件呈现（sw_device）可上屏跑 vkmark | 原生 swapchain、Wayland/KMS、Android surface、dma-buf 导出/导入实际使用 |
| 计算 | 仅内部 clear 使用；应用级 compute pipeline/dispatch/SSBO/原子 | 全部未验证 |
| GL | — | **被挡**：无 `/dev/dri/renderD*`、kbase 非 DRM，panfrost Gallium 无法挂接 |

已知限制汇总：①内核无 dma-buf exporter，WSI 只能走软件呈现且需 `PANVK_KBASE_WSI_SW=1`；②扩展/feature 声明极窄，依赖扩展的应用无法运行；③无 render node ⇒ GL 不可用；④异步提交非默认；⑤无 TILER_OOM 处理路径（5 处 heap 跳过是必需）；⑥vkmark 在 `vkAcquireNextImageKHR` 上有一条 semaphore 复用的 Validation 报错（未修）。

## M2-A 最小应用选型（2026-10-19）

| 应用 | 结论 | 证据 / 原因 |
|---|---|---|
| `vkcube`（vulkan-tools 1.4.304） | ⭐ **选用并跑通** | 经典最小 demo：单 render pass、单 UBO 描述符、深度缓冲、无 compute/MSAA/动态渲染；只需 `VK_KHR_swapchain`（`PANVK_KBASE_WSI_SW=1` 时声明）与 xcb surface。60 帧 `--c 60` 退出码 0；30 帧 `--validate` 退出码 0 且 0 条 Validation Error；长跑时 `xwininfo` 确认映射出 512×512 窗口（`0x200000 ... 512x512+100+100`） | `/tmp/m2a_vkcube.log`、`/tmp/m2a_vkcube_validate.log`、`xwininfo` |
| `vkcubepp` | 可跑 | 同参数 60 帧退出码 0 | `/tmp/m2a_vkcubepp.log` |
| `vkgears` | 失败（记录，不深挖） | 打印 conformant 警告后立即 **SIGSEGV**（rc=139），未进入设备/交换链输出；疑与其所需 feature/扩展在当前 kbase 声明下为零有关，按约定不调查 | `/tmp/m2a_vkgears.log` |
| `vulkaninfo` | 仅诊断工具 | 已安装，不作为 workload | — |
| SaschaWillems samples / vkQuake 等 | 不在本机 | 需联网获取并构建，且多数依赖未声明 feature/扩展；未采用 | — |

运行环境与 vkmark 一致：`DISPLAY=:110`、`VK_ICD_FILENAMES=/tmp/kbase-test-icd.json`、`DISABLE_WSI_LAYER=1`、`PANVK_KBASE_WSI_SW=1`、`PANVK_KBASE_DEVICE=/dev/mali0`、`PAN_KMOD_RESTRICT_TO_BACKENDS=kbase`、`XDG_RUNTIME_DIR=/tmp`。命令：`vkcube --c N --suppress_popups --width 512 --height 512 --present_mode 2 --wsi xcb`。

M2-A 结论：在 apiVersion 1.0 + 当前 feature 集 + X11 软件呈现下存在可运行的最小应用，**无需退到 B（compute + SSBO）**。

## M2-B compute dispatch + SSBO 最小闭环（2026-10-19）

| 项 | 内容 |
|---|---|
| 探针位置 | `mesa-port-backups/m2b-compute-20261019/`：`comp.comp`（GLSL）、`m2b_compute_probe.c`、`comp.spv`、`m2b_compute_probe` |
| 设计 | 256 个 u32 的 SSBO，初始 `values[i]=i`；`local_size_x=64`、dispatch 4 组；shader 做 `values[i] = values[i]*2 + 1`；提交计算队列，`vkWaitForFences`（5 s 超时）+ `vkQueueWaitIdle` 后 CPU 回读逐项比对 |
| 判据 | expected = `2*i+1`；observed 逐项比较，`mismatch=0/256` 为 PASS。若 dispatch 未执行，observed 应为 `i`，全部 256 项不匹配，判据可区分 |
| 结果 | ⭐ **PASS 6/6**（首次 + 5 次重复），全部 `mismatch=0/256`。`OBSERVED first8=1,3,5,7,9,11,13,15` 与 `EXPECTED` 一致 |
| 环境 | `VK_ICD_FILENAMES=/tmp/kbase-test-icd.json`、`PANVK_KBASE_DEVICE=/dev/mali0`、`PAN_KMOD_RESTRICT_TO_BACKENDS=kbase`；无 surface/swapchain，无 WSI |
| 设备信息 | `Mali-G925-Immortalis MC12 api=1.0.0`；唯一队列族 `index=0 flags=0x7`（GRAPHICS/COMPUTE/TRANSFER） |
| 缺口 | 未遇到未声明 feature/扩展；`pEnabledFeatures=NULL`、`enabledExtensionCount=0` 即可建 compute pipeline |
| 覆盖边界 | 已验证：compute pipeline 创建、STORAGE_BUFFER 描述符绑定、dispatch 执行、SSBO 读改写、host 回读、fence/queue 同步。未验证：device-local SSBO、间接 dispatch、原子/workgroup/subgroup、dispatch 与其他阶段的 GPU 侧屏障、多 dispatch 链；API 失败路径未覆盖 |
| 日志 | `/tmp/m2b_run1.log`、`/tmp/m2b_rep_1..5.log` |

### vkgears 崩溃记录（只记现象，不深挖）

| 日期 | 现象 | 环境 |
|---|---|---|
| 2026-10-19 | `vkgears -size 512x512` 打印 `WARNING: panvk is not a conformant Vulkan implementation, testing use only.` 后**立即 SIGSEGV（rc=139）**，未输出设备/交换链信息 | 与 vkmark/vkcube 相同 kbase 环境（X11 软件呈现、apiVersion 1.0、feature 仅 4 项）；日志 `/tmp/m2a_vkgears.log`。按约定不调查；疑与其所需 feature/扩展在当前声明下为零有关，未验证 |

## M3-A 异步提交重新评估（2026-10-19）

| 场景 | 模式 | 次数 | 通过 | 耗时 | 失败形态 |
|---|---|---|---|---|---|
| m2b compute（256×u32 SSBO + dispatch） | sync（变量未设） | 5 | 5/5 | 17–35 ms（中位 ~22） | — |
| 同上 | async（`PANVK_KBASE_ASYNC_SUBMIT=1`） | 5 | 5/5 | 27–34 ms（中位 ~29） | — |
| vkcube `--c 60` 512×512 fifo | sync | 5 | 5/5 | 583–632 ms（首跑 288 ms 为离群值） | — |
| 同上 | async | 5 | 5/5 | 613–673 ms（约 +3%） | — |

- CPU 时间（bash `time`，compute，各 3 次）：sync `real≈0.025 user≈0.004–0.008 sys≈0.011–0.016`；async `real≈0.021–0.025 user≈0.003–0.008 sys≈0.012–0.014`。**无 CPU 等待减少的可测量证据**——微基准总耗时仅 20–30 ms，dispatch 延迟主导，本来就没有可省的等待。
- vkcube 改为异步后帧时间无改善（约 +3%，在噪声边缘），与之前 "SHM 单帧必须等" 的判断一致。
- **残余卡死形态**：本轮 20 次运行**未出现** fence 永不就绪，也未出现 `DEVICE_LOST`/超时。若后续出现，形态应为进程挂起直到超时（async 下没有 5 s 的 completion 超时翻译）。
- ⚠️ **开关语义**：代码是 `getenv("PANVK_KBASE_ASYNC_SUBMIT") != NULL`（`panvk_kbase_compute_queue.inc:623`），**只要变量存在就开异步**，`=0` 不会关闭。本轮 sync 对照用的是 `unset`，不是 `=0`。后续脚本必须用 unset。
- 结论：**不改默认值**，异步仍 opt-in。没有证据支持在 compute 或 vkcube 场景默认开启。
- 日志：`/tmp/m3_compute_{sync,async}_1..5.log`、`/tmp/m3_vkcube_{sync,async}_1..5.log`。

## 异步开关语义修复（2026-10-19）

| 项 | 内容 |
|---|---|
| 修复 | `PANVK_KBASE_ASYNC_SUBMIT` 改为显式解析：未设或非 `"0"` → 开；`"0"` → 关。位置 `panvk_kbase_compute_queue.inc:623` 及 `<string.h>` |
| 验证（gdb 断点 `panvk_kbase_sync_set_pending`） | `=0`：**不命中**，进程正常退出（sync 路径）；未设：命中，栈为 `kbase_compute_submit → panvk_kbase_sync_set_pending`（async 路径）；`=false`：命中（非 `"0"` 视为开启） |
| 默认值 | 未改：不设变量仍为异步（与修复前行为一致） |

## M3-B GL 路径静态评估（2026-10-19，不实施）

| 问题 | 结论 |
|---|---|
| `pan_device.c` ENOTSUP 挡的是什么 | `panfrost_open_device()` 在 `pan_kmod_dev_create()` 之后、读 GPU props 之前，若后端是 `kbase_kmod_ops` 直接 `errno=ENOTSUP; goto err`（`pan_device.c:45-50`）。唯一调用者是 `panfrost_create_screen()`（`pan_screen.c:1046`），其入口是由 DRI loader 用 DRM 设备名打开的 `panfrost_drm_screen_create()`（`panfrost_drm_winsys.c:126`、`dri_util.c:1118`）。⇒ **它挡住的是整个 Gallium screen 创建**，不只是某一条特性路径 |
| 解开前置条件 | ①把 kbase fd 送进 Gallium 的入口（本机无 `renderD*`，loader 找不到设备；需 env/平台入口打开 `/dev/mali0`）；②kbase 提交后端；③kbase 同步后端；④`bo_wait`/`bo_export`；⑤WSI 决策（见下） |
| DRM 假设 | Gallium CSF 直接用 panthor ioctl：`DRM_IOCTL_PANTHOR_GROUP_SUBMIT`（`pan_csf.c:554-566`）、`GROUP_CREATE`（`:1978`）、`GROUP_GET_STATE`（`:797`）；这些经 `pan_kmod_ioctl` 原样下发，kbase UAPI 完全不同，**不能靠 ifdef 翻译** |
| mmap / VM | `kbase_kmod_bo_mmap` 可用（WB 映射）；但 `kbase_kmod_bo_wait` 是 ENOTSUP（`kbase_kmod.c:593-598`）、`bo_export` ENOTSUP（`:547-553`）；`vm_bind` 仅支持立即模式（`:1008-1009`），部分 VM op 类型 ENOTSUP（`:1020,:1047`） |
| 同步 | kbase 的 `util_sync_provider` **全部 ENOTSUP**（`kbase_sync.c`：create/destroy/handle↔fd/import/export/wait/reset/signal/transfer）。Gallium CSF 依赖 `pan_kmod_sync_*` 与 `ctx->syncobj`（`pan_csf.c:751-752,2096,2137`）⇒ 现有 fence/WSI 路径无法工作 |
| WSI | GL winsys 走 DRM dumb buffer + `drmPrimeHandleToFD` 导出（`panfrost_drm_winsys.c:100-110`）与 renderonly；内核无 dma-buf exporter、无 DRM 节点 ⇒ 只能走软件 SHM 呈现（与 Vulkan `sw_device` 同思路） |
| 可复用 | panfrost 编译器/NIR、格式与 blend 表、`cs_builder` + `pan_csf.c` 的 CS 生成、`pan_kmod` BO/VM 分配、`kbase_csf` 的 queue/ring/doorbell/wait、kbase heap 续期、panvk 的同步概念 |
| 必须新写 | kbase winsys/screen 入口与 fd 注入；kbase 提交后端（把 CS 发布到 queue group，替换 panthor group ioctl）；kbase `util_sync_provider` 真实实现；Gallium fence 对接；`bo_wait`；上屏时的 SHM 呈现（或先只做 offscreen） |
| 工作量粗估 | **Offscreen GL（surfaceless EGL + FBO 绘制 + 回读）**：约 3–5k 行新增/改动，单人+真机约 4–8 周，风险高；**上屏 X11 GL**：再加 2–4k 行、约 2–4 周，风险高且无零拷贝；**完整应用兼容/一致性**不在估算内 |
| 替代路线 | Zink（GL over Vulkan）：当前 kbase Vulkan 的扩展表几乎全零、feature 仅 4 项，Zink 不可行；若走此路需先做一轮能力声明扩展，属另一项工程 |
| 最大风险 | Gallium CSF 后端在架构上就是 panthor UAPI 形状；缺的不是几个 ifdef，而是提交与同步两套后端，且 GL state tracker 的正确性要求远高于目前 panvk 已验证子集 |
| 结论 | 静态可行但代价高；不建议作为近期里程碑，除非 GL 成为硬需求。本评估未写代码、未做动态实验 |

## 一键演示包（2026-10-19）

| 文件 | 作用 |
|---|---|
| `mesa-port-backups/demo-20261019/build.sh` | 构建 ICD（ninja + 拷贝）、生成绝对路径 ICD json、编译 compute 探针与 SPIR-V |
| `env.sh` | 统一环境变量：DISPLAY、XDG_RUNTIME_DIR、VK_ICD_FILENAMES、DISABLE_WSI_LAYER、PANVK_KBASE_WSI_SW、PANVK_KBASE_DEVICE、PAN_KMOD_RESTRICT_TO_BACKENDS |
| `run.sh` | 一条命令依次跑 vkmark vertex、vkcube 60 帧、compute 探针，逐项 PASS/FAIL + 总耗时，日志 `/tmp/demo_<item>.log` |
| `compute/` | `comp.comp`、`m2b_compute_probe.c`（SSBO 读改写 + 精确回读） |
| `KNOWN-LIMITATIONS.md` | WSI/扩展/异步/同步缓解/TILER_OOM/GL 等已知限制 |
| `PROJECT-STATUS.md` | 定位、能力清单、三条路线成本收益 |

已验证：`build.sh` 在真机环境成功产出 ICD、SPIR-V 与探针可执行文件。

## 长时稳定性（2026-10-19）

| 测试 | 结果 | 失败形态 |
|---|---|---|
| vkcube 10 分钟（`--c 100000000`, 512×512 fifo） | **失败，约 540 s 崩溃** | SIGABRT：`vkcube: ./cube/cube.c:1327: demo_draw: Assertion '!err' failed`；窗口在 60 s 存在、540 s 消失；vkcube 不打印具体 Vulkan 错误值 |
| compute 探针 1000 次 | **1000/1000 通过**（26 s，`mismatch=0/256`） | 无 |
| vkmark vertex 100 轮 | **99/100 通过**；FPS min 229 / max 334 / mean 290 | run 67：`kbase completion failed (VK_ERROR_DEVICE_LOST)` → `vk::DeviceLostError`（已知残余同步卡死） |

⭐ **init 16 工程缓解下残余卡死复现**：vkmark 100 轮中 1 次（约 1%）。按此前决策规则，这满足“init 16 下再次复现”的重新追根因条件；本轮按任务边界未调查，留待决定。

演示包验证：`demo-20261019/run.sh` 一条命令 3/3 通过（vkmark 2081 ms、vkcube 719 ms、compute 27 ms，总 2841 ms）。包内 `lib/` 与构建产物、`/tmp/kbase-icd` 三处二进制 `cmp` 一致。

## vkcube 长时崩溃取证与 vkmark 1% 处置（2026-10-19）

| 项 | 内容 |
|---|---|
| 取证方式 | 本地构建 Vulkan-Tools `vulkan-sdk-1.4.304.1`（`/tmp/vt-src-304`），只在 `demo_draw` 的 present `else` 分支断言前加一行错误码打印（`VKCUBE_PRESENT_ERROR`），不改驱动 |
| 构建 | `cmake -DUPDATE_DEPS=ON -DBUILD_CUBE=ON -DBUILD_WSI_XLIB_SUPPORT=ON -DBUILD_WSI_XCB_SUPPORT=ON`，产物 `/tmp/vt-src-304/build/cube/vkcube`；60 帧 sanity rc=0 |
| 10 分钟结果 | **未复现崩溃**：`rc=124`（timeout 到点仍在运行）、窗口在 60 s 与 540 s 均存在、日志无 `VKCUBE_PRESENT_ERROR` |
| 结论 | 分发版 vkcube 的一次 ~540 s SIGABRT（`demo_draw` 断言 `!err`）为**非确定性、未复现、错误码未知**；patched 本地版单次 10 分钟通过不足以断言已消失。按指示记录后停，不深挖 |
| vkmark 1% | 已在 `KNOWN-LIMITATIONS.md` 标注 **“known, accepted, not pursued”**；不再调查 |
| 边界 | 未改驱动核心代码；未碰 WSI/U7/级 C；未动 `panvk_kbase_sync_type` 与 5 处 heap 跳过 |
