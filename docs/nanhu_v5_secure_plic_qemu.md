# Nanhu V5 QEMU PLIC 安全扩展说明



## 1. 安全扩展寄存器

以下偏移均相对于 PLIC base。当前 QEMU V5 PLIC base 为 `0x3c000000`。

| 寄存器 | 偏移 | 作用 | 复位值 |
|---|---:|---|---:|
| `SEC_SRC` | `0x4000` | 每个 source 的安全属性，0 为 Non-secure，1 为 Secure | 0 |
| `SEC_CTRL` | `0x4100` | bit0 Enable，bit1 Lock | 0 |
| `WORLD_STATE` | `0x4200` | 每个 hart 的当前 S-world 只读视图，0 为 REE，1 为 TEE | 1 |
| `IRQ_TRACK` | `0x4600` | 每个 context 的在服 IRQ、安全属性和有效位 | 0 |
| `SEC_STATUS` | `0x4e00` | Complete 拒绝状态 | 0 |

`SEC_CTRL.LOCK` 置位后保持到 PLIC 复位。锁定期间不能修改 Enable 和
`SEC_SRC`。

### 1.1 MWS 世界状态接口

最新 RTL 不再允许软件通过 PLIC `WORLD_STATE` MMIO 修改世界状态，也删除了
`0x4400` 的 `WS_ACK`。每个 Nanhu V5 hart 增加 M 态自定义 CSR `mws(0x3ff)`：

```text
mws.WS = 0  REE
mws.WS = 1  TEE
reset  = 1  TEE
```

`mws.WS` 从 CPU 输出，经 SoC 时钟域同步后直接送入 PLIC。PLIC 内部再进行两级
采样，并把采样结果暴露在只读 `WORLD_STATE` 中。QEMU 使用 CPU 命名 GPIO
`mws` 建模该硬件连线；OpenSBI 通过 CSR 写入切换世界，不再写 PLIC MMIO，
也不再等待软件可见的 ACK。

OpenSBI 写 `mws` 时仅暂时清除本 hart 的 `mie.MEIE`，写入后恢复原值。这一操作
用于避免世界切换临界区被 M external interrupt 打断，不代表存在旧版 ACK
握手。

### 1.2 SEC_SRC 位编号

当前实现对齐 Nanhu core
[`c11437b6`](https://github.com/cuihu2/nanhu-v5-s/commit/c11437b65655a26b56a6d92e85363c13df7370c9)
和 LinkNan
[`4fb5c89f`](https://github.com/cuihu2/linknan-s/commit/4fb5c89f87d3ddc612795aa7e7ee73669ada1c7b)
所采用的 PLIC 安全接口。
`SEC_SRC` 与 pending/enable 一样，使用 IRQ ID 直接作为位编号，bit0 保留：

```text
word_offset = 0x4000 + 4 * (irq / 32)
bit_mask    = 1 << (irq % 32)
```

| IRQ | SEC_SRC 偏移 | 位掩码 |
|---|---:|---:|
| 1 | `0x4000` | `0x00000002` |
| 31 | `0x4000` | `0x80000000` |
| 32 | `0x4004` | `0x00000001` |
| 40 | `0x4004` | `0x00000100` |
| 41 | `0x4004` | `0x00000200` |
| 255 | `0x401c` | `0x80000000` |

旧版使用 `irq - 1` 计算位编号，新版必须同时更新 QEMU 与 OpenSBI。
只更新其中一侧会导致安全源错位。DTS 仍声明 IRQ ID，例如
`riscv,secure-irqs = <41>`，不需要改成位掩码。

QEMU 对保留的 bit0 和未实现的 source 位忽略写入；SEC_SRC 分配长度包含
保留的 ID0。claim 锁存的 `IRQ_TRACK.req_sec` 使用同一个 ID 对应的安全属性。
新版 RTL 还修复了 claim 属性索引错位，但原 QEMU 已按当前 IRQ 查询属性，
因此这部分无需新增处理路径。

安全扩展迁移子节版本升级为 2，不接受旧版位图格式的快照或迁移状态。
升级时应退出旧 QEMU，重新编译并冷启动，不恢复旧快照。

## 2. 路由规则

对给定 hart、context 和 source：

```text
to_S = (sec_src[id] == world_state[hart])

to_M = !to_S && !(sec_busy_hart && !sec_src[id])
```

`sec_busy_hart` 表示该 hart 的任意 PLIC context 正在服务尚未 Complete 的
安全中断：

```text
sec_busy_hart = irq_track.in_service && irq_track.req_sec
```

`to_S` 和 `to_M` 只是安全路由条件。真正产生外部中断还必须同时满足原生
PLIC 条件：

```text
effective_S = pending && enable_S && priority_pass && to_S
effective_M = pending && enable_M && priority_pass && to_M
```

因此，“`to_M = 1`”不等于 OpenSBI 一定收到 M external interrupt。如果
M-context 没有 enable 对应 source，中断仍保持 pending。

## 3. 四类场景的当前处理方式

| 当前 world | IRQ 属性 | QEMU 路由结果 | 当前软件实际效果 |
|---|---|---|---|
| REE | Non-secure | S-context | Linux claim、handler、complete |
| REE | Secure | M-context | OpenSBI claim，进入 OP-TEE FIQ，OpenSBI complete |
| TEE | Secure | S-context | OP-TEE 可直接 claim；IRQ41 当前无功能 handler |
| TEE | Non-secure | M-context 候选；secure-busy 时阻断 | M 未 enable 普通 IRQ，保持 pending，返回 REE 后由 Linux 处理 |

### 3.1 REE + Non-secure IRQ

以 REE 期间到来 IRQ40 为例：

```text
world_state = 0
sec_src[40] = 0
to_S = 1
to_M = 0
```

QEMU 将 IRQ40 路由到 hart0 S-context。Linux 已经 enable IRQ40，因此当前
实际路径为：

```text
UART0 IRQ40
  -> PLIC S external interrupt
  -> Linux PLIC claim
  -> Linux UART handler
  -> Linux PLIC complete，安全属性位为 0
```

### 3.2 REE + Secure IRQ

以 REE 期间到来 IRQ41 为例：

```text
world_state = 0
sec_src[41] = 1
to_S = 0
to_M = 1
```

OpenSBI 当前为 M-context enable 了 DTS 中声明的安全 IRQ，因此硬件能够产生
M external interrupt。当前软件路径为：

```text
UART1 IRQ41
  -> PLIC M external interrupt
  -> OpenSBI M-context claim IRQ41
  -> OpenSBI 保存 REE domain context
  -> OpenSBI 进入 OP-TEE FIQ entry
  -> OP-TEE 返回 FIQ_DONE
  -> OpenSBI M-context secure complete IRQ41
  -> OpenSBI 恢复 REE domain context
```

### 3.3 TEE + Secure IRQ

以 OP-TEE 正常运行期间到来 IRQ41 为例：

```text
world_state = 1
sec_src[41] = 1
to_S = 1
to_M = 0
```

OpenSBI 在进入 TEE 时会为 S-context enable 配置的安全 IRQ，因此 QEMU 可以
直接向 OP-TEE 所在的 S-context 产生外部中断：

```text
UART1 IRQ41
  -> PLIC S external interrupt
  -> OP-TEE S-context claim IRQ41
  -> OP-TEE interrupt_main_handler
  -> OP-TEE secure complete，Complete bit31 为 1
```

### 3.4 TEE + Non-secure IRQ

以 OP-TEE 正常运行期间到来 IRQ40 为例：

```text
world_state = 1
sec_src[40] = 0
to_S = 0
to_M = 1                  when sec_busy_hart = 0
to_M = 0                  when sec_busy_hart = 1
```

从 QEMU 硬件规则看，IRQ40 应成为 M-context 的候选中断。但当前 OpenSBI 只为
M-context enable 安全 IRQ，没有 enable IRQ40，因此实际结果为：

```text
IRQ40 pending
  -> S-context 因 world 不匹配而不可见
  -> M-context 因 enable_M[40] = 0 而不产生 MEIP
  -> OP-TEE 正常运行并返回
  -> OpenSBI 设置 world_state = REE
  -> IRQ40 重新匹配 S-context
  -> Linux claim、handler、complete
```

当前实现属于“延迟到 REE 再处理”

> 注：当为该非安全源配置 `riscv,ree-preempt-irqs` 后，TEE 可被其**主动抢占**，
> 由 OpenSBI 重入 OP-TEE 挂起安全线程再切回 REE 处理。详见
> [`opensbi_optee_foreign_intr_preemption.md`](./opensbi_optee_foreign_intr_preemption.md)。

## 4. 构建与回归验证

本次适配将 world state 更新方式从“PLIC MMIO 写入并轮询 WS_ACK”替换为
“OpenSBI 写 CPU `mws` CSR，CPU 硬件信号驱动 PLIC”。world state 初值、路由
公式、complete 安全标志保持不变，也不实现 TEE 被非安全 IRQ 抢占后切回
REE 的软件协议。Linux 和 OP-TEE 的中断处理逻辑保持不变。

> 后续工作：TEE 被非安全 IRQ 抢占并切回 REE 的软件协议已在
> [`opensbi_optee_foreign_intr_preemption.md`](./opensbi_optee_foreign_intr_preemption.md)
> 中单独设计实现（OpenSBI 侧改动 + 一条 DTS 属性，OP-TEE/Linux 无需改动）。

在顶层目录构建 QEMU、OpenSBI，并执行独立的硬件模型回归测试：

```sh
make qemu opensbi-jump NPROC=8
QTEST_QEMU_BINARY="$PWD/build/qemu/qemu-system-riscv64" \
    ./build/qemu/tests/qtest/nanhu-secure-plic-test
```

测试不需要启动 Linux，也不修改 guest 镜像。8 项测试覆盖：

- IRQ1/31/32/41/255 的属性读写与相邻源隔离，包含跨 32-bit 字边界。
- REE/TEE 下安全与非安全源的 M/S claim 路由及 `IRQ_TRACK.req_sec`。
- 无重复 claim、错误安全标志 complete 拒绝、合法 complete 和重放拒绝。
- `SEC_CTRL.LOCK`、保留 bit0，以及安全中断在服时非安全 M claim 被阻断。
- `WORLD_STATE` 只能由 `mws` 硬件输入更新，MMIO 写入不能改变状态。

运行系统级冒烟测试：

```sh
make run-tee NPROC=8
```

进入 Linux 后，以 root 执行：

```sh
devmem 0x3c004100 32
devmem 0x3c004004 32
devmem 0x3c004200 32
cat /proc/interrupts
ls -l /dev/tee*
pidof tee-supplicant
xtest 1006
xtest 1009
```

在当前 DTS 只配置 IRQ41 为安全源的情况下，前三项读值依次为
`0x00000003`、`0x00000200`、`0x00000000`。
其中第二项不再是旧版的 `0x00000100`；新编号下该值表示 IRQ41 为安全源。
Linux 串口仍使用非安全 IRQ40，TEE 设备和 tee-supplicant 应正常可用。
QTest 验证的是硬件模型，不等同于四种场景的跨世界软件端到端验证；
上述 xtest 也只是选定用例的冒烟测试，不代表全量测试通过。

### 4.1 最新 RTL 接口适配验证记录

2026-10-21，使用上述构建和启动命令，在单 hart Nanhu V5 QEMU 上实测：

| 检查项 | 结果 |
|---|---|
| QEMU / OpenSBI / OP-TEE / Linux 编译 | 通过 |
| secure PLIC QTest | 8/8 通过 |
| `WORLD_STATE` MMIO 只读 | 通过；MMIO 写入不改变状态 |
| `mws` CSR 切换 PLIC world state | 通过；QTest 写 CSR 并覆盖 0/1 两种状态 |
| Linux / OP-TEE 启动 | 进入 Buildroot root shell，OP-TEE driver 初始化成功 |
| SEC_CTRL / SEC_SRC 第二字 / WORLD_STATE | `0x3` / `0x200` / `0x0` |
| MWS 端到端证据 | CPU 复位值为 1，OpenSBI 切换到 REE 后 Linux 读到 PLIC 状态 0 |
| TEE 用户态服务 | `/dev/tee0`、`/dev/teepriv0` 存在，tee-supplicant PID 为 82 |
| xtest 1006 | 通过，0 failed |

启动时等待 `eth0` 超时是当前板级网络设备未提供导致的既有现象，不影响本次
PLIC、串口与 TEE 冒烟验证。

### 4.2 历史验证记录

2026-10-08，使用上述构建和启动命令，在单 hart Nanhu V5 QEMU 上实测：

| 检查项 | 结果 |
|---|---|
| QEMU / OpenSBI 编译 | 通过 |
| secure PLIC QTest | 7/7 通过 |
| Linux / OP-TEE 启动 | 进入 Buildroot root shell，OP-TEE driver 初始化成功 |
| SEC_CTRL / SEC_SRC 第二字 / WORLD_STATE | `0x3` / `0x200` / `0x0` |
| Linux UART0 | `/proc/interrupts` 显示 PLIC IRQ40，串口交互正常 |
| TEE 用户态服务 | `/dev/tee0`、`/dev/teepriv0` 存在，tee-supplicant 运行 |
| xtest 1006 | 通过，0 failed |
| xtest 1009 | 1009.1、1009.2、1009.4 通过；1009.3 失败 |
| 测试后 SEC_STATUS | `0x0`，未记录 complete 拒绝 |

`1009.3` 的实际返回值是 `TEEC_SUCCESS`，而测试期望 `TEEC_ERROR_CANCEL`。
现象与此前提供的取消测试失败日志一致，本次未修改相关逻辑，也未通过修改前后
对照实验确定该失败的根因。不能据此宣称全量 xtest 通过。
启动时仍有等待 `eth0` 超时的网络服务报错，不影响本次串口和 TEE 冒烟测试。
