# Nanhu V5 QEMU PLIC 安全扩展说明



## 1. 安全扩展寄存器

以下偏移均相对于 PLIC base。当前 QEMU V5 PLIC base 为 `0x3c000000`。

| 寄存器 | 偏移 | 作用 | 复位值 |
|---|---:|---|---:|
| `SEC_SRC` | `0x4000` | 每个 source 的安全属性，0 为 Non-secure，1 为 Secure | 0 |
| `SEC_CTRL` | `0x4100` | bit0 Enable，bit1 Lock | 0 |
| `WORLD_STATE` | `0x4200` | 每个 hart 的当前 S-world，0 为 REE，1 为 TEE | 1 |
| `WS_ACK` | `0x4400` | world state 写入完成和回显 | `done + state` |
| `IRQ_TRACK` | `0x4600` | 每个 context 的在服 IRQ、安全属性和有效位 | 0 |
| `SEC_STATUS` | `0x4e00` | Complete 拒绝状态 | 0 |

`SEC_CTRL.LOCK` 置位后保持到 PLIC 复位。锁定期间不能修改 Enable 和
`SEC_SRC`。

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
