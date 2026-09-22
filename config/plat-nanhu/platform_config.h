/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright 2022-2023 NXP
 *
 * Brief   OpenXiangShan Nanhu V5 platform configuration.
 */

#ifndef PLATFORM_CONFIG_H
#define PLATFORM_CONFIG_H

#include <mm/generic_ram_layout.h>
#include <riscv.h>

/* The stack pointer is always kept 16-byte aligned */
#define STACK_ALIGNMENT		16

/* DRAM */
#ifndef DRAM_BASE
#define DRAM_BASE		 0x80000000
#define DRAM_SIZE		0x200000000 /* 8GB */
#endif

/* CLINT */
#ifndef CLINT_BASE
#define CLINT_BASE		0x38000000
#endif

/* PLIC */
#ifndef PLIC_BASE
#define PLIC_BASE		0x3c000000
#define PLIC_REG_SIZE		0x4000000
#define PLIC_NUM_SOURCES	255
#endif

/* UART0 */
#ifndef UART0_BASE
#define UART0_BASE		0x310B0000
#endif
#define UART0_IRQ		40

#define PLAT_THREAD_EXCP_FOREIGN_INTR	\
	(CSR_XIE_EIE | CSR_XIE_TIE | CSR_XIE_SIE)
#define PLAT_THREAD_EXCP_NATIVE_INTR	(0)

#endif /*PLATFORM_CONFIG_H*/
