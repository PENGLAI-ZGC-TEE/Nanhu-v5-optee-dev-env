# ISA extension flags
$(call force,CFG_RV64_core,y)
$(call force,CFG_RISCV_ISA_C,y)
$(call force,CFG_RISCV_FPU,y)

# Core flags
CFG_DT := y
$(call force,CFG_CORE_LARGE_PHYS_ADDR,y)
$(call force,CFG_CORE_RESERVED_SHM,n)
$(call force,CFG_CORE_DYN_SHM,y)

# Crypto flags
$(call force,CFG_WITH_SOFTWARE_PRNG,y)
$(call force,CFG_RISCV_ZKR_RNG,n)

# Protection flags
$(call force,CFG_CORE_ASLR,n)
$(call force,CFG_WITH_STACK_CANARIES,n)

# Hart-related flags
CFG_TEE_CORE_NB_CORE ?= 1
CFG_NUM_THREADS ?= 1
$(call force,CFG_BOOT_SYNC_CPU,n)

# SBI-related flags
$(call force,CFG_RISCV_M_MODE,n)
$(call force,CFG_RISCV_S_MODE,y)
$(call force,CFG_RISCV_SBI,y)
$(call force,CFG_RISCV_WITH_M_MODE_SM,y)

# Device flags
$(call force,CFG_RISCV_PLIC,y)
$(call force,CFG_NANHU_PLIC_SECURE_EXT,y)
$(call force,CFG_RISCV_SBI_CONSOLE,y)
$(call force,CFG_16550_UART,n)
$(call force,CFG_RISCV_TIME_SOURCE_RDTIME,y) # Maybe Nanhu-v3a doesn't support 0xC01 CSR
CFG_RISCV_MTIME_RATE := 1000000

# TA-related flags
supported-ta-targets := ta_rv64

# Memory layout flags
CFG_TDDRAM_START := 0x81000000
CFG_TDDRAM_SIZE  :=  0x1000000
CFG_TEE_RAM_VA_SIZE ?= 0x00200000
