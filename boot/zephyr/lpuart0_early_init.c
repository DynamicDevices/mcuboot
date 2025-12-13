/*
 * Copyright 2024 Dynamic Devices Ltd
 * SPDX-License-Identifier: Apache-2.0
 *
 * LPUART0 pin configuration for bootloader console
 * Configures PTD6/PTD7 for LPUART0 if needed
 */

#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/kernel.h>

#ifdef CONFIG_UART_CONSOLE

/**
 * @brief Post-kernel LPUART0 pin configuration (POST_KERNEL)
 * 
 * This runs after Zephyr's pinctrl system has had a chance to configure pins,
 * but we verify and fix pin configuration if needed.
 */
static int lpuart0_post_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	
	/* PORTD PCR base address: 0x4004C000 (from reference manual) */
	#define PORTD_PORT_BASE 0x4004C000
	#define PORTD_PCR6_ADDR (PORTD_PORT_BASE + (6 * 4))
	#define PORTD_PCR7_ADDR (PORTD_PORT_BASE + (7 * 4))
	
	/* Check current pin configuration */
	uint32_t pcr6 = sys_read32(PORTD_PCR6_ADDR);
	uint32_t pcr7 = sys_read32(PORTD_PCR7_ADDR);
	uint32_t mux6 = (pcr6 >> 8) & 0x7;
	uint32_t mux7 = (pcr7 >> 8) & 0x7;
	
	/* Check if pins need configuration */
	if (mux6 != 3 || mux7 != 3) {
		/* Enable Port D clock */
		#define SIM_SCGC5_ADDR 0x40048038
		#define SIM_SCGC5_PORTD_BIT (1 << 13)
		uint32_t scgc5 = sys_read32(SIM_SCGC5_ADDR);
		if (!(scgc5 & SIM_SCGC5_PORTD_BIT)) {
			sys_write32(scgc5 | SIM_SCGC5_PORTD_BIT, SIM_SCGC5_ADDR);
			k_busy_wait(1000);
		}
		
		/* Configure pins to Alt 3 (LPUART0) */
		sys_write32(0x300, PORTD_PCR6_ADDR);
		k_busy_wait(100);
		sys_write32(0x300, PORTD_PCR7_ADDR);
		k_busy_wait(100);
	}
	
	return 0;
}

SYS_INIT(lpuart0_post_init, POST_KERNEL, 50);

#endif /* CONFIG_UART_CONSOLE */
