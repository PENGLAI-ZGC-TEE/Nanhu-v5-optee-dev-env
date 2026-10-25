# OpenSBI ↔ OP-TEE 安全世界抢占接口设计

本文档描述“安全世界运行期间被非安全（foreign）中断抢占”的处理流程，以及
OpenSBI 与 OP-TEE 之间的交互接口设计与对应代码修改。它是
[`nanhu_v5_secure_plic_qemu.md`](./nanhu_v5_secure_plic_qemu.md) 中第 3.4 节
（TEE + Non-secure IRQ）由“延迟到 REE 再处理”升级为“主动抢占”的落地方案。

## 1. 问题定义

- 设备被静态划分为安全/非安全，产生安全/非安全中断。
- Linux 运行在非安全世界（REE），处理非安全中断；OP-TEE 运行在安全世界
  （TEE），处理安全中断；M 态固件 OpenSBI 负责世界切换。
- PLIC 感知每个中断源的安全属性和每个 hart 的世界状态。当中断的安全属性与
  hart 世界状态不匹配（foreign interrupt）时，PLIC 把中断投递到 M-context，
  使 hart 陷入 M 态交给 OpenSBI。

目标场景：**hart 处于 TEE 世界、OP-TEE 正在执行某个安全线程时，到来一个非安全
中断**。按路由规则该中断成为 foreign interrupt，被投递到 M-context：

```text
world_state = 1 (TEE)
sec_src[irq] = 0 (non-secure)
to_S = (sec_src == world_state) = 0
to_M = !to_S && !(sec_busy_hart && !sec_src) = 1   (无安全中断在服时)
```

期望行为：

1. OpenSBI 重入 OP-TEE，令其**挂起被中断的安全线程**（保存其完整上下文）。
2. OP-TEE 通过 `ecall` 把控制权交还 OpenSBI。
3. OpenSBI 完成世界切换到 REE。
4. Linux 处理该非安全中断，随后恢复被挂起的安全线程。

这与 ARM TrustZone 上 OP-TEE 的 foreign interrupt / RPC 返回语义一致：安全世界
被非安全中断抢占后，以一次“RPC 返回”退回普通世界，由普通世界处理中断，之后
通过“RPC 返回继续”恢复安全线程。本设计是其 RISC-V / PLIC + MWS 版本。

## 2. 设计原则：复用 OP-TEE 既有机制

OP-TEE RISC-V 内核**已经具备**被 foreign 中断抢占所需的全部机制：

- `thread_trap_vect`（S 态陷入向量）会区分 native / foreign 中断。
  在本平台（`plat-nanhu`）的分类为：

  ```c
  /* config/plat-nanhu/platform_config.h */
  #define PLAT_THREAD_EXCP_FOREIGN_INTR  (CSR_XIE_TIE | CSR_XIE_SIE)  /* 定时器/软件 */
  #define PLAT_THREAD_EXCP_NATIVE_INTR   (CSR_XIE_EIE)                /* 外部(PLIC) */
  ```

  即**外部中断被视为 native（安全中断，场景 3.3），定时器/软件中断被视为
  foreign**。

- `thread_foreign_interrupt_handler()`：保存被中断线程上下文、调用
  `thread_state_suspend(THREAD_FLAGS_EXIT_ON_FOREIGN_INTR, status, pc)`
  将线程置为挂起，然后 `thread_return_to_udomain()` 以
  `a0 = TEEABI_OPTEED_RETURN_CALL_DONE`、`a1 = OPTEE_ABI_RETURN_RPC_FOREIGN_INTR`
  发起 MPXY `ecall` 返回普通世界。
- `thread_resume_from_rpc()` / `thread_resume()`：普通世界通过标准
  “return from RPC”路径再次进入 OP-TEE 时，恢复挂起的安全线程。

Linux 侧（`drivers/tee/optee/smc_abi.c` 的通用 RPC 循环）也**已经**支持
`OPTEE_SMC_RETURN_RPC_FOREIGN_INTR`：收到后无需额外动作（返回普通世界即由
正常中断向量吃掉该中断），随后以 `OPTEE_SMC_CALL_RETURN_FROM_RPC` 重入
OP-TEE 恢复安全线程。

因此本设计的核心，是让 OpenSBI 在 M 态收到 foreign interrupt 后，**通过
OP-TEE 向量表中的一个专用入口向量（dedicated entry vector）重入 OP-TEE**，由该
向量复用上述挂起/恢复逻辑。

> 结论：**Linux 无需修改代码**；OP-TEE 仅新增一个专用入口向量
> `vector_foreign_intr_entry`（向量表 slot 9），其余逻辑复用既有实现。新增的
> 抢占判定与世界切换逻辑集中在 OpenSBI 的 PLIC irqchip 驱动中（外加一条 DTS
> 属性）。
>
> 本分支（`foreign-intr-dedicated-vector`）与基线分支
> （`opensbi_extension_dev`，在 `stvec` 注入虚拟陷入）相比，把“哪种 `scause`
> 属于 foreign”的平台知识从 OpenSBI 移入 OP-TEE，使 OpenSBI 与中断分类解耦。

## 3. 交互接口定义（OpenSBI ↔ OP-TEE ABI）

交互复用既有的 MPXY/TEE-ABI 通道，不引入新的 ecall 号或 MPXY 消息号；唯一新增
的 ABI 元素是 OP-TEE 向量表中的专用入口向量（slot 9）。接口由三部分构成：

### 3.1 M→S 重入（OpenSBI → OP-TEE，经专用入口向量）

OpenSBI 在 M 态把被中断上下文“摆放”成一次陷入的样子，并 `mret` 跳转到 OP-TEE
向量表的**专用入口向量** `vector_foreign_intr_entry`（slot 9）：

| 项目 | 取值 | 说明 |
|---|---|---|
| `sepc` | 被中断的 OP-TEE PC（原 `mepc`） | 入口向量的保存逻辑读取 `XEPC`，线程恢复点 |
| `sstatus.SPP` | 被中断特权级（S→1 / U→0） | 决定恢复到 S 还是 U |
| `sstatus.SPIE` | 进入向量前的 `sstatus.SIE` | 镜像硬件陷入语义 |
| `sstatus.SIE` | `0` | 屏蔽 S 态中断，镜像硬件陷入语义 |
| `mepc`（返回目标） | `opteed_get_foreign_intr_entry()`（slot 9 地址） | `mret` 进入专用入口向量 |
| `mstatus.MPP` | `PRV_S` | `mret` 后进入 S 态执行 OP-TEE |
| 通用寄存器 | 保持被中断值 | 由入口向量复用的陷入保存逻辑负责保存 |

**OpenSBI 不写 `scause`。** 入口向量在 OP-TEE 内部合成一个被分类为 foreign 的
cause（本平台为监管者软件中断，落在 `PLAT_THREAD_EXCP_FOREIGN_INTR = SIE|TIE`
内），随后复用 OP-TEE 既有的陷入保存与 foreign 分发路径
（`interrupt_from_kernel` / `interrupt_from_user` →
`thread_foreign_interrupt_handler`）。因此“哪种中断属于 foreign”这一平台知识
留在 OP-TEE（其 plat 配置处），OpenSBI 完全不感知。

入口向量复用既有陷入保存的关键点：

- 通过 `XSCRATCH` 的 kernel(=0)/user(!=0) 约定区分被抢占于内核态还是用户态
  （与 `thread_trap_vect` 一致），OpenSBI 不触碰 `XSCRATCH`。
- 先把 `sp/a0/a1` 暂存到 `thread_core_local`，再用已被暂存的 `a0` 合成
  `scause`，避免破坏被中断线程的任何寄存器（真实陷入由硬件写 `scause`，此处由
  软件在“寄存器已暂存之后”写，等价且无副作用）。

同时 OpenSBI **屏蔽本 hart 的 `mie.MEIE`**：由于未对该 foreign 源执行
claim，`MEIP` 仍然有效；若不屏蔽，`mret` 进入 S 态 OP-TEE 的瞬间会立刻再次
陷入 M 态（S 态下 M 态中断恒使能），形成死循环。

OP-TEE `thread_vector_table` 的槽位布局（4 字节跳转表，OpenSBI 按槽号偏移寻址，
两侧必须保持一致）：

| slot | 偏移 | 入口 | 使用方 |
|---:|---:|---|---|
| 0 | `+0x00` | `vector_std_abi_entry` | 既有：yield 调用 |
| 1 | `+0x04` | `vector_fast_abi_entry` | 既有：fast 调用 |
| 6 | `+0x18` | `vector_fiq_entry` | 既有：REE + 安全中断（场景 3.2） |
| **9** | **`+0x24`** | **`vector_foreign_intr_entry`** | **本设计新增：TEE 被 foreign 中断抢占** |

OpenSBI 通过 `opteed_get_foreign_intr_entry()`（= 向量表基址 + `9 * 4`）取得
slot 9 地址，与既有 `opteed_get_fiq_entry()`（slot 6）同构。

### 3.2 S→M 返回（OP-TEE → OpenSBI，“ecall 归还”）

OP-TEE 的 foreign 处理结束后，经既有
`thread_return_to_udomain()` 发起 MPXY `ecall`（`OPTEED_MSG_COMPLETE`），
寄存器约定（既有 ABI）：

```text
a0 = TEEABI_OPTEED_RETURN_CALL_DONE   (0xBE000005)
a1 = OPTEE_ABI_RETURN_RPC_FOREIGN_INTR (0xFFFF0004)   -> 透传给 REE 的 res.a0
a2..a7 = 恢复信息（thread 索引等），必须保留并透传
```

OpenSBI 的 `mpxy_opteed_send_message()`（`OPTEED_MSG_COMPLETE` 分支）把
`a1..a4` 拷入 REE 共享内存作为返回值，`fiq_done` 为假，执行
`sbi_ecall_tee_domain_exit(false)`：先 `sbi_domain_context_exit()` 退回 REE
（udomain）上下文，再 `fdt_plic_set_current_world_state(false)` 切回 REE 世界。
**此路径与普通 RPC 返回完全一致，无需在 opteed dispatcher 中新增分支。**

### 3.3 世界切换与中断再路由

`fdt_plic_set_current_world_state(false)` 写 `mws` CSR 把世界切回 REE。世界
翻转后，仍处于 pending 的非安全源的路由由 `to_M` 变为 `to_S`，PLIC 自动把它
投递到 S-context（Linux）。随后 OpenSBI **重新使能先前被屏蔽的 `mie.MEIE`**，
保证后续 REE 下的安全中断（场景 3.2）仍能进入 M 态。

> 关键：OpenSBI 全程**不 claim** 该 foreign 源。claim 会清除 pending 并占用
> M-context 的 in-service 槽，使 Linux 无法再 claim。依赖“世界翻转后 PLIC 自动
> 改路由 + 源仍 pending”即可把中断干净地交给 Linux，无需任何 QEMU/RTL 改动。

## 4. 端到端时序

```text
Linux(REE) --MPXY COMMUNICATE--> OpenSBI --context_enter(tdomain), world=TEE--> OP-TEE
                                                                                  │ 运行安全线程(TA)
        非安全 IRQ40 到来, world=TEE => to_M=1 => MEIP                            │
OP-TEE <--------------------- M 态陷入 ---------------------- OpenSBI plic_secure_irqfn
  │                         (world==TEE ⇒ foreign 抢占)
  │  OpenSBI: 屏蔽 mie.MEIE; 摆放被中断上下文(sepc=被中断PC, SPP=被中断特权);
  │           mret 跳入 OP-TEE 专用入口向量 vector_foreign_intr_entry (slot 9)
  ▼
vector_foreign_intr_entry: 合成 foreign cause → 复用陷入保存
  → interrupt_from_{kernel,user} → thread_foreign_interrupt_handler
  │  挂起当前安全线程 (thread_state_suspend, EXIT_ON_FOREIGN_INTR)
  │  thread_return_to_udomain: a0=CALL_DONE, a1=RPC_FOREIGN_INTR
  ▼ ecall (MPXY COMPLETE)
OpenSBI: 拷贝返回值; context_exit→REE; world=REE(写 mws);
         重新使能 mie.MEIE;  (IRQ40 现在 to_S=1 → 投递 S-context)
  ▼ mret
Linux(REE): MPXY 调用返回 res.a0=RPC_FOREIGN_INTR
  │  S 外部中断 pending → Linux claim/handler/complete 处理 IRQ40
  │  optee_handle_rpc: FOREIGN_INTR 分支; a0=RETURN_FROM_RPC
  ▼ MPXY COMMUNICATE (return from RPC)
OpenSBI --context_enter(tdomain), world=TEE--> OP-TEE
  ▼
thread_resume_from_rpc → thread_resume: 恢复被挂起的安全线程, 继续执行
```

## 5. 触发条件：让 foreign 中断到达 M-context

默认 OpenSBI 只在 M-context 使能 DTS 声明的安全源，因此 TEE 下的非安全中断不会
产生 `MEIP`（即第 3.4 节“延迟到 REE”的根因）。要实现抢占，需在 M-context
使能**允许抢占 TEE 的非安全源**。

新增 DTS 属性 `riscv,ree-preempt-irqs`（声明 IRQ ID 列表）：

```dts
plic: interrupt-controller@3c000000 {
        ...
        riscv,secure-irqs = <41>;        /* 安全源 */
        riscv,ree-preempt-irqs = <40>;   /* 允许抢占 TEE 的非安全源(UART0) */
};
```

OpenSBI 在 cold init 解析该属性（校验必须为非安全源），在 warm init 把这些源
使能到 M-context 并设置其 priority>0；M-context threshold 保持 0。

由于路由规则完全由硬件门控：

- REE + 非安全：`to_S=1, to_M=0` → 只到 Linux（场景 3.1 不变）。
- TEE + 非安全：`to_M=1` → 进入 M（本抢占流程）。

因此把这些源常驻使能在 M-context 是安全的，仅在 TEE 世界才会真正触发。

> 选用“显式 DTS 列表”而非“动态镜像 S-context 使能位”，是为了确定性与可测试
> 性，并避免某非安全源被 M 使能、却未被 Linux 的 S-context 使能时，在 TEE 下
> 反复抢占却无人处理而形成 livelock。列表中应只放 Linux 确实会处理的源。

## 6. 可抢占性判定（安全性约束）

经专用入口向量重入会绕过 OP-TEE 自身的中断屏蔽，因此必须保证被抢占时存在
“可挂起的当前线程”，否则 `thread_state_suspend()` 的 `curr_thread` 断言会失败。
OpenSBI 在重入前判定：

- 被中断于 **U 态**（TA 运行）：恒可挂起 → 抢占。
- 被中断于 **S 态且重入前 `sstatus.SIE==1`**（线程活跃、不在临界区/tmp 栈）：
  可挂起 → 抢占。
- 其它情况（S 态且 `SIE==0`，典型是进入/退出线程的粘合代码运行在 tmp 栈、
  `curr_thread` 无效）：**不抢占，延迟处理**。延迟时仍屏蔽 `mie.MEIE` 并打标记，
  在下一次切回 REE 时（任意 COMPLETE 路径）重新使能 `MEIE`，该中断即被投递给
  Linux。这与既有“延迟到 REE 再处理”的行为一致。

## 7. 代码修改清单

### 7.1 OpenSBI

`lib/utils/irqchip/fdt_irqchip_plic.c`：

- 新增 `preempt_config`（`riscv,ree-preempt-irqs` 解析结果）与
  `plic_foreign_preempt_meie_masked[]`（每 hart 的 MEIE 屏蔽标记）。
- 新增 `plic_foreign_intr_preempt()`：可抢占性判定 + 屏蔽 `MEIE` + 摆放被中断
  上下文（`sepc`/`SPP`）并把 `mepc` 指向 OP-TEE 专用入口向量（见 §3.1）。
- `plic_secure_irqfn()`：当 `world_state==TEE` 时转入上述抢占路径（TEE 下任何
  M 外部中断都是 foreign 抢占）。
- `fdt_plic_set_current_world_state(false)`：切回 REE 后按标记重新使能 `MEIE`。
- `irqchip_plic_warm_init()`：把 `preempt_config` 源使能到 M-context。
- `irqchip_plic_cold_init()`：解析 `riscv,ree-preempt-irqs`。

`lib/utils/mpxy/fdt_mpxy_opteed.c`（及头文件）：新增
`opteed_get_foreign_intr_entry()`，返回向量表 slot 9 地址。

`config/nanhu-v5.dts`：PLIC 节点新增 `riscv,ree-preempt-irqs = <40>;`。

### 7.2 OP-TEE

新增**专用入口向量** `vector_foreign_intr_entry`：

- `core/arch/riscv/kernel/thread_rv.S`：新增该向量，复现陷入前导（kernel/user
  区分 + `sp/a0/a1` 暂存），合成 foreign cause，跳入既有
  `interrupt_from_kernel` / `interrupt_from_user` 保存与分发路径。
- `core/arch/riscv/kernel/thread_optee_abi_rv.S`：在 `thread_vector_table`
  slot 9 增加 `j vector_foreign_intr_entry`。

挂起/恢复仍复用既有 `thread_foreign_interrupt_handler` /
`thread_resume_from_rpc`。foreign/native 的分类（`PLAT_THREAD_EXCP_FOREIGN_INTR`）
仍由 `config/plat-nanhu/platform_config.h` 决定，但该知识被封装在入口向量内。

### 7.3 Linux

**无代码改动。** 通用 RPC 循环已处理 `OPTEE_SMC_RETURN_RPC_FOREIGN_INTR`
（返回即吃中断，随后 `RETURN_FROM_RPC` 恢复安全线程）。

## 8. 构建与验证

### 8.1 构建

```sh
make qemu opensbi-jump optee_os linux-image NPROC=8
```

OpenSBI 单组件编译验证：

```sh
make opensbi-jump NPROC=8        # 已验证通过（含新 DTS 属性的 DTB 一并生成）
```

### 8.2 系统级冒烟（建议）

```sh
make run-tee NPROC=8
```

进入 Linux 后，验证 M-context 已使能抢占源、安全/用户态 TEE 服务正常：

```sh
devmem 0x3c004200 32     # WORLD_STATE: REE 下应为 0
cat /proc/interrupts     # 确认 PLIC IRQ40(UART0) 存在
ls -l /dev/tee*; pidof tee-supplicant
xtest 1006
```

### 8.3 抢占路径的功能验证：preempt_test

测试代码位于 `tests/preempt_ta/`：

| 文件 | 作用 |
|---|---|
| `include/preempt_test_ta.h` | UUID、命令号，以及 TA 与 host 共用的计算函数 |
| `ta/` | 测试 TA（`65dca01d-cd82-4b02-98bb-c980a3657545`），在 U 态长时间自旋计算 |
| `host/main.c` | host 程序 `preempt_test`，直接使用 Linux TEE uapi，静态链接，不依赖 rootfs 中 libteec/glibc 的版本 |
| `scripts/analyze_int_log.py` | 分析 QEMU `-d int` 日志，统计抢占证据 |
| `scripts/run_qemu_test.py` | 无人值守：启动 QEMU、运行测试、采集并分析日志 |

**设计要点**

- **长时间运行于 TA 用户态**：TA 在 U 态对 16 个 64 位状态字做混合运算，默认
  每轮 2 亿次迭代（QEMU 上约 2.6 s）。U 态被抢占时总是满足 §6 的可抢占条件。
- **检测上下文保存/恢复错误**：每次迭代末尾用空 `asm` 加 `"+r"` 约束，强制 16 个
  状态字都驻留在通用寄存器中。线程被挂起、恢复之后，只要有任一寄存器没有正确
  保存或恢复，最终校验和就会改变；host 在普通世界用同一函数重新计算并比对。
- **用异步的接收中断做激励**：TA 运行期间要有人往 UART0 控制台打字。RX 中断
  （IRQ40）与 Linux 的执行时机无关，绝大多数会在 hart 处于 TEE 世界时到来。
  测试程序自身的控制台输出所产生的 TX 中断不行：它们在 Linux 执行 `write()`
  后立即产生，此时 hart 处在 REE，直接走 S-context，实测完全触发不了抢占路径。
  `run_qemu_test.py` 在 TA 运行期间每 20 ms 输入一个空格，结束后用 `Ctrl-U`
  清除。
- **直接的抢占证据**：本平台没有 Sstc，S 态定时器由 OpenSBI 置位 `mip.STIP`；
  而在 `plat-nanhu` 上，定时器中断对 OP-TEE 属于 foreign 中断。实测表明原有的
  定时器路径本身就会周期性地让 TA 退回 REE，因此“TA 运行期间串口仍有响应”不能
  证明新路径生效。测试期间唯一的安全源 UART1（IRQ41）空闲，场景 3.2 不会发生，
  所以日志窗口内**每一次 M 外部中断（async cause 11）都只能来自本文的抢占
  路径**。`analyze_int_log.py` 还会检查其后紧接的陷入是否为 OP-TEE 核心发出的
  S 态 ecall（即线程挂起后的 MPXY COMPLETE）。

**运行方式**

```sh
make preempt-test-install      # 编译 TA 与 host，并追加到 rootfs cpio
make linux-image               # 重新打包 initramfs
python3 tests/preempt_ta/scripts/run_qemu_test.py     # 无人值守运行
```

`preempt-test-install` 每次都从 tracked 的 `config/rootfs_nopasswd.cpio.gz`
重新生成基础 cpio，再追加一个只含测试文件的 cpio（内核会依次解包拼接的 cpio），
因此可以重复执行，也不会修改 tracked 的压缩包。

也可以手动运行：`make run-tee` 进入 shell 后，按 `Ctrl-a c` 进入 QEMU monitor
执行 `log int`，再按 `Ctrl-a c` 返回；执行 `preempt_test`（参数 `-n 迭代次数
-r 轮数`），**运行期间在控制台持续敲键**；结束后在 monitor 中执行
`log guest_errors`，再运行
`python3 tests/preempt_ta/scripts/analyze_int_log.py guest_log.txt`。

**判定标准**

1. 每轮输出 `checksum ... OK`，最终输出 `preempt_test: PASSED`；
2. 分析结果中 `M external interrupts (cause 11)` 大于 0，并且大部分之后紧跟
   OP-TEE 的 S 态 ecall；
3. 对照实验：去掉 DTS 中的 `riscv,ree-preempt-irqs` 后重跑，cause 11 计数应为 0。

### 8.4 验证记录

2026-10-22，`foreign-intr-dedicated-vector` 分支（含 mstatus 修复），单 hart
Nanhu V5 QEMU，`run_qemu_test.py` 默认参数（3 轮 × 2 亿次迭代，每 20 ms 输入
一个字符）：

| 检查项 | 启用抢占 | 对照（去掉 `riscv,ree-preempt-irqs`） |
|---|---|---|
| M-context enable 字（`0x3c002004`） | `0x300`（IRQ40、41） | `0x200`（仅 IRQ41） |
| 校验和 | 3/3 一致，PASSED | 一致，PASSED |
| M 外部中断（cause 11） | 235（TA 用户态 209，OP-TEE 内核态 26） | 0 |
| 其后紧跟 OP-TEE S 态 ecall | 214 | 0 |
| S 外部中断（cause 9） | 929 | 1305 |

TA 在约 7.9 s 内被新路径抢占、挂起、恢复两百余次，16 个寄存器驻留的状态全部
正确。cause 11 之后没有紧跟 ecall 的约 20 次全部落在 OP-TEE 内核态，与 §6 中
“S 态且 `SIE==0` 时延迟处理”的分支一致。对照组同样通过，此时 IRQ40 只能在定时器
等其他原因返回 REE 时才得到处理。

> 对照实验注意：OpenSBI 通过 `FW_FDT_PATH` 把 DTB 嵌入固件。改回原 DTS 重建时，
> 如果 `build/nanhu-v5.dtb` 比固件目标文件旧，OpenSBI 不会重新嵌入，需要先
> `touch build/nanhu-v5.dtb`（或 `make opensbi-clean`）。可以用
> `devmem 0x3c002004 32` 确认 M-context 的 enable 位。

## 9. 设计取舍与边界

- **专用入口向量 vs 向 `stvec` 注入**：本分支采用专用入口向量
  `vector_foreign_intr_entry`，与既有 `vector_fiq_entry` 模式对称，入口语义显式；
  并把“哪种 `scause` 属于 foreign”的平台知识封装进 OP-TEE，使 OpenSBI 与中断
  分类解耦。代价是 OP-TEE 侧新增一段入口汇编；该汇编通过跳入既有
  `interrupt_from_kernel` / `interrupt_from_user` 复用成熟的寄存器保存逻辑，
  仅新增前导（kernel/user 区分 + 暂存 + 合成 cause），风险可控。基线分支
  （`opensbi_extension_dev`）则直接向 `stvec` 注入虚拟陷入、OP-TEE 零改动，但
  OpenSBI 需感知平台的 foreign cause 编码。
- **为何不 claim foreign 源**：claim 会破坏“世界翻转后由 Linux 接管”的干净路径
  （见 §3.3），且 level 触发源的 re-pend 存在时序竞争。
- **单源在服约束**：若此刻存在未 complete 的安全中断（`sec_busy_hart`），按路由
  规则 `to_M` 被阻断，foreign 中断暂不投递到 M，自然延后到安全中断 complete 后，
  不会破坏安全中断处理。
- **多 hart**：所有状态按 hartindex 分离；当前 `plat-nanhu` 配置为单核。
- **平台耦合**：§3.1 中 foreign cause 的合成（监管者软件中断）依赖本平台
  foreign/native 分类，该逻辑位于 OP-TEE `vector_foreign_intr_entry`。移植到
  foreign 含外部中断的平台（如 `plat-spike`）时，只需修改该入口向量合成的
  cause，OpenSBI 无需改动。
