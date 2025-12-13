/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 * Copyright (c) 2020 Arm Limited
 * Copyright (c) 2021-2023 Nordic Semiconductor ASA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/printk.h>
#include <soc.h>
#include <zephyr/linker/linker-defs.h>
#if defined(CONFIG_CPU_CORTEX_M)
#include <cmsis_core.h>
#endif

#include "target.h"
#include "bootutil/bootutil_log.h"
#include <flash_map_backend/flash_map_backend.h>
#include <sysflash/sysflash.h>

#if defined(CONFIG_BOOT_SERIAL_PIN_RESET) || defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
#include <zephyr/drivers/hwinfo.h>
#endif

#if defined(CONFIG_BOOT_SERIAL_BOOT_MODE) || defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif

/* Validate serial recovery configuration */
#ifdef CONFIG_MCUBOOT_SERIAL
#if !defined(CONFIG_BOOT_SERIAL_ENTRANCE_GPIO) && \
    !defined(CONFIG_BOOT_SERIAL_WAIT_FOR_DFU) && \
    !defined(CONFIG_BOOT_SERIAL_BOOT_MODE) && \
    !defined(CONFIG_BOOT_SERIAL_NO_APPLICATION) && \
    !defined(CONFIG_BOOT_SERIAL_PIN_RESET)
#error "Serial recovery selected without an entrance mode set"
#endif
#endif

/* Validate firmware loader configuration */
#ifdef CONFIG_BOOT_FIRMWARE_LOADER
#if !defined(CONFIG_BOOT_FIRMWARE_LOADER_ENTRANCE_GPIO) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_NO_APPLICATION) && \
    !defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
#error "Firmware loader selected without an entrance mode set"
#endif
#endif

#ifdef CONFIG_MCUBOOT_INDICATION_LED

/*
 * The led0 devicetree alias is optional. If present, we'll use it
 * to turn on the LED whenever the button is pressed.
 */
#if DT_NODE_EXISTS(DT_ALIAS(mcuboot_led0))
#define LED0_NODE DT_ALIAS(mcuboot_led0)
#endif

#if DT_NODE_HAS_STATUS(LED0_NODE, okay) && DT_NODE_HAS_PROP(LED0_NODE, gpios)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
#else
/* A build error here means your board isn't set up to drive an LED. */
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

BOOT_LOG_MODULE_DECLARE(mcuboot);

void io_led_init(void)
{
    if (!device_is_ready(led0.port)) {
        BOOT_LOG_ERR("Didn't find LED device referred by the LED0_NODE\n");
        return;
    }

    gpio_pin_configure_dt(&led0, GPIO_OUTPUT);
    gpio_pin_set_dt(&led0, 0);
}

void io_led_set(int value)
{
    gpio_pin_set_dt(&led0, value);
}
#endif /* CONFIG_MCUBOOT_INDICATION_LED */

#if defined(CONFIG_BOOT_SERIAL_ENTRANCE_GPIO) || defined(CONFIG_BOOT_USB_DFU_GPIO) || \
    defined(CONFIG_BOOT_FIRMWARE_LOADER_ENTRANCE_GPIO)

#if defined(CONFIG_MCUBOOT_SERIAL)
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_SERIAL_DETECT_DELAY
#elif defined(CONFIG_BOOT_FIRMWARE_LOADER)
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_FIRMWARE_LOADER_DETECT_DELAY
#else
#define BUTTON_0_DETECT_DELAY CONFIG_BOOT_USB_DFU_DETECT_DELAY
#endif

#define BUTTON_0_NODE DT_ALIAS(mcuboot_button0)

#if DT_NODE_EXISTS(BUTTON_0_NODE) && DT_NODE_HAS_PROP(BUTTON_0_NODE, gpios)
static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(BUTTON_0_NODE, gpios);
#else
#error "Serial recovery/USB DFU button must be declared in device tree as 'mcuboot_button0'"
#endif

bool io_detect_pin(void)
{
    int rc;
    int pin_active;

    if (!device_is_ready(button0.port)) {
        __ASSERT(false, "GPIO device is not ready.\n");
        return false;
    }

    rc = gpio_pin_configure_dt(&button0, GPIO_INPUT);
    __ASSERT(rc == 0, "Failed to initialize boot detect pin.\n");

    rc = gpio_pin_get_dt(&button0);
    pin_active = rc;

    __ASSERT(rc >= 0, "Failed to read boot detect pin.\n");

    if (pin_active) {
        if (BUTTON_0_DETECT_DELAY > 0) {
#ifdef CONFIG_MULTITHREADING
            k_sleep(K_MSEC(50));
#else
            k_busy_wait(50000);
#endif

            /* Get the uptime for debounce purposes. */
            int64_t timestamp = k_uptime_get();

            for(;;) {
                rc = gpio_pin_get_dt(&button0);
                pin_active = rc;
                __ASSERT(rc >= 0, "Failed to read boot detect pin.\n");

                /* Get delta from when this started */
                uint32_t delta = k_uptime_get() -  timestamp;

                /* If not pressed OR if pressed > debounce period, stop. */
                if (delta >= BUTTON_0_DETECT_DELAY || !pin_active) {
                    break;
                }

                /* Delay 1 ms */
#ifdef CONFIG_MULTITHREADING
                k_sleep(K_MSEC(1));
#else
                k_busy_wait(1000);
#endif
            }
        }
    }

    return (bool)pin_active;
}
#endif

#if defined(CONFIG_BOOT_SERIAL_PIN_RESET) || defined(CONFIG_BOOT_FIRMWARE_LOADER_PIN_RESET)
bool io_detect_pin_reset(void)
{
    uint32_t reset_cause;
    int rc;

    rc = hwinfo_get_reset_cause(&reset_cause);

    if (rc == 0 && (reset_cause & RESET_PIN)) {
        (void)hwinfo_clear_reset_cause();
        return true;
    }

    return false;
}
#endif

#if defined(CONFIG_BOOT_SERIAL_BOOT_MODE) || defined(CONFIG_BOOT_FIRMWARE_LOADER_BOOT_MODE)
bool io_detect_boot_mode(void)
{
    int32_t boot_mode;

    boot_mode = bootmode_check(BOOT_MODE_TYPE_BOOTLOADER);

    if (boot_mode == 1) {
        /* Boot mode to stay in bootloader, clear status and enter serial
         * recovery mode
         */
        bootmode_clear();

        return true;
    }

    return false;
}
#endif

/* Bootloader flag structure - must match application's bootloader_comm.h */
#define BOOTLOADER_FLAG_OFFSET 0x100UL  /* Offset in storage partition (matches application) */
#define BOOTLOADER_FLAG_MAGIC  0xB00710ADUL  /* "BOOTLOAD" in hex-speak */

typedef struct {
    uint32_t magic;         /* Magic number for integrity */
    uint8_t timeout_s;      /* Timeout in seconds for DFU wait mode (0-255) */
    uint8_t reserved[3];    /* Reserved for future use */
} bootloader_flag_t;

/* Check flash storage partition for bootloader entry request from application */
/* This flag survives both warm and cold resets */
/* Returns timeout in seconds (0-255) if flag detected, -1 if not detected */
int io_detect_ram_bootloader_request(void)
{
    const struct flash_area *fa;
    int ret;
    bootloader_flag_t flag_value = {0};
    
    /* Check if storage partition exists */
    #if FIXED_PARTITION_EXISTS(storage_partition)
    printk("D: Storage partition exists, attempting to open...\n");
    ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
    if (ret < 0) {
        printk("E: Failed to open storage partition for bootloader flag check: %d\n", ret);
        return -1;
    }
    printk("D: Storage partition opened successfully, reading flag at offset 0x%lx...\n", (unsigned long)BOOTLOADER_FLAG_OFFSET);
    
    /* Read flag structure from flash */
    ret = flash_area_read(fa, BOOTLOADER_FLAG_OFFSET, &flag_value, sizeof(bootloader_flag_t));
    flash_area_close(fa);
    
    if (ret < 0) {
        printk("E: Failed to read bootloader flag from flash: %d\n", ret);
        return -1;
    }
    
    if (flag_value.magic == BOOTLOADER_FLAG_MAGIC) {
        /* Log message with D: prefix for debug-level logging */
        printk("D: Flash bootloader request detected (magic: 0x%x, timeout: %d s), entering serial recovery\n",
               (unsigned int)flag_value.magic, flag_value.timeout_s);
        
        /* Clear the flag so it doesn't trigger again */
        ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
        if (ret == 0) {
            /* Erase and write zero to clear flag */
            flash_area_erase(fa, BOOTLOADER_FLAG_OFFSET, sizeof(bootloader_flag_t));
            bootloader_flag_t zero_flag = {0};
            flash_area_write(fa, BOOTLOADER_FLAG_OFFSET, &zero_flag, sizeof(bootloader_flag_t));
            flash_area_close(fa);
        } else {
            printk("E: Failed to open storage partition to clear bootloader flag: %d\n", ret);
        }
        
        /* Return timeout value (0-255) */
        return (int)flag_value.timeout_s;
    }
    #else
    printk("E: Storage partition not found in device tree - cannot check bootloader flag\n");
    #endif
    
    return -1;  /* Flag not detected */
}

/* Power rail initialization and logging */

/* System configuration structure (must match application's config_storage.h) */
#define CONFIG_STORAGE_MAGIC  0x53594346UL  /* "SYCF" */
#define CONFIG_STORAGE_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t crc;
    struct {
        uint16_t low_battery_threshold_mv;
        uint16_t critical_battery_threshold_mv;
        uint32_t rtc_wake_interval_minutes;
        uint32_t battery_check_interval_hours;
        bool imx93_power_control;
        bool nfc_wake_enabled;
        bool uart_wake_enabled;
        uint16_t imx93_power_on_delay_ms;
        uint16_t wifi_power_on_delay_ms;
        uint16_t display_power_on_delay_ms;
        struct {
            bool pmic_en_default;
            bool wifi_en_default;
            bool wifi_buf_en_default;
            bool disp_en_default;
        } power_rail_defaults;
    } power;
    /* Skip rest of structure - we only need power_rail_defaults */
    uint8_t _padding[100];  /* Padding to ensure we can read the power_rail_defaults */
} system_config_t;

/* Store power rail states for later logging (after console is ready) */
static struct {
    bool initialized;
    bool pmic_en;
    bool wifi_en;
    bool wifi_buf_en;
    bool disp_en;
    bool from_flash;
} power_rail_state = {0};

/* GPIO register definitions for power rail control */
#define SIM_SCGC5_ADDR       0x40048038  /* System Integration Module Clock Gating Control 5 */
#define SIM_SCGC5_PORTE_BIT (1 << 13)   /* Bit 13 = PORTE clock enable */
#define PORTE_BASE_ADDR      0x400FF100
#define PORTE_PCR17_OFFSET   (17 * 4)   /* 17 pins * 4 bytes per PCR */
#define PORTE_PCR16_OFFSET   (16 * 4)   /* 16 pins * 4 bytes per PCR */
#define PORTE_PCR19_OFFSET   (19 * 4)   /* 19 pins * 4 bytes per PCR */
#define PORTE_PCR18_OFFSET   (18 * 4)   /* 18 pins * 4 bytes per PCR */
#define PORTE_PCR17_ADDR     (PORTE_BASE_ADDR + PORTE_PCR17_OFFSET)
#define PORTE_PCR16_ADDR     (PORTE_BASE_ADDR + PORTE_PCR16_OFFSET)
#define PORTE_PCR19_ADDR     (PORTE_BASE_ADDR + PORTE_PCR19_OFFSET)
#define PORTE_PCR18_ADDR     (PORTE_BASE_ADDR + PORTE_PCR18_OFFSET)
#define PCR_PULL_UP_GPIO     0xC1       /* PE=1 (bit 6), PS=1 (bit 7), MUX=1 (bits 0-2) */
#define PCR_OUTPUT_GPIO      0x101      /* PE=1 (bit 6), PS=0 (bit 7), MUX=1 (bits 0-2), ODE=1 (bit 11) */
#define GPIOE_BASE_ADDR      0x400FF100
#define PMIC_EN_PIN          17  /* PTE17 */
#define DISP_EN_PIN          16  /* PTE16 */
#define WIFI_EN_PIN          19  /* PTE19 */
#define WIFI_BUF_EN_PIN      18  /* PTE18 */

/* Read power rail defaults from flash storage */
static int read_power_rail_defaults(bool *pmic_en, bool *wifi_en, bool *wifi_buf_en, bool *disp_en)
{
    const struct flash_area *fa;
    int ret;
    system_config_t config = {0};
    
    /* Default values (if config not found) */
    *pmic_en = true;
    *wifi_en = true;
    *wifi_buf_en = true;
    *disp_en = false;
    
    /* Check if storage partition exists */
    #if FIXED_PARTITION_EXISTS(storage_partition)
    printk("D: Reading power rail defaults from storage partition...\n");
    ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
    if (ret < 0) {
        printk("E: Failed to open storage partition for power rail defaults: %d\n", ret);
        return -1;
    }
    printk("D: Storage partition opened for power rails, reading config...\n");
    
    /* Read system configuration from flash (offset 0x0) */
    ret = flash_area_read(fa, 0, &config, sizeof(system_config_t));
    flash_area_close(fa);
    
    if (ret < 0) {
        printk("E: Failed to read power rail defaults from flash: %d\n", ret);
        return ret;
    }
    
    /* Validate magic number */
    if (config.magic != CONFIG_STORAGE_MAGIC) {
        printk("E: Invalid storage magic number: 0x%x (expected 0x%x)\n", 
               (unsigned int)config.magic, (unsigned int)CONFIG_STORAGE_MAGIC);
        return -1;
    }
    
    /* Extract power rail defaults */
    *pmic_en = config.power.power_rail_defaults.pmic_en_default;
    *wifi_en = config.power.power_rail_defaults.wifi_en_default;
    *wifi_buf_en = config.power.power_rail_defaults.wifi_buf_en_default;
    *disp_en = config.power.power_rail_defaults.disp_en_default;
    
    printk("D: Power rail defaults read from flash: PMIC=%d WiFi=%d WiFiBuf=%d Disp=%d\n",
           *pmic_en, *wifi_en, *wifi_buf_en, *disp_en);
    
    return 0;
    #else
    printk("E: Storage partition not found in device tree - using default power rail values\n");
    return -1;
    #endif
}

/* Enable PORTE clock once - called before configuring any GPIO pins */
static void enable_porte_clock(void)
{
    /* CRITICAL: Enable PORTE clock before accessing GPIO registers */
    uint32_t sim_scgc5 = sys_read32(SIM_SCGC5_ADDR);
    if (!(sim_scgc5 & SIM_SCGC5_PORTE_BIT)) {
        sim_scgc5 |= SIM_SCGC5_PORTE_BIT;
        sys_write32(sim_scgc5, SIM_SCGC5_ADDR);
        
        /* CRITICAL: Memory barrier to ensure write completes before reading back */
        __DSB();
        __ISB();
        
        /* CRITICAL: Wait for clock to stabilize before accessing registers */
        /* Use longer delay - clock enable can take time, especially early in boot */
        volatile int i;
        for (i = 0; i < 10000; i++) {
            __asm__("nop");
        }
        
        /* Verify clock is actually enabled */
        sim_scgc5 = sys_read32(SIM_SCGC5_ADDR);
        if (!(sim_scgc5 & SIM_SCGC5_PORTE_BIT)) {
            /* Clock enable failed - this is a critical error */
            printk("E: Failed to enable PORTE clock!\n");
            return;
        }
        
        /* Additional delay after verification to ensure clock is fully stable */
        for (i = 0; i < 5000; i++) {
            __asm__("nop");
        }
    } else {
        /* Clock already enabled - still add a small delay for stability */
        volatile int i;
        for (i = 0; i < 1000; i++) {
            __asm__("nop");
        }
    }
}

/* Configure GPIO pin as output and set state */
/* NOTE: enable_porte_clock() must be called once before calling this function */
static void configure_gpio_output(uint32_t port_base, uint32_t pcr_addr, uint32_t pin_mask, bool state)
{
    /* Verify clock is enabled (defensive check) */
    uint32_t sim_scgc5 = sys_read32(SIM_SCGC5_ADDR);
    if (!(sim_scgc5 & SIM_SCGC5_PORTE_BIT)) {
        printk("E: PORTE clock not enabled before GPIO access!\n");
        return;
    }
    
    /* Configure pin as GPIO output (push-pull) */
    /* MUX=1 (GPIO), PE=1 (pull enable), PS=0 (pull down), ODE=0 (push-pull) */
    sys_write32(PCR_OUTPUT_GPIO, pcr_addr);
    
    /* Small delay after PCR write */
    volatile int i;
    for (i = 0; i < 200; i++) {
        __asm__("nop");
    }
    
    /* Set output state via GPIO Port Data Output Register (PDOR) */
    uint32_t pdor = sys_read32(port_base + 0x00);  /* PDOR */
    if (state) {
        pdor |= pin_mask;
    } else {
        pdor &= ~pin_mask;
    }
    sys_write32(pdor, port_base + 0x00);
    
    /* Ensure write completes */
    for (i = 0; i < 100; i++) {
        __asm__("nop");
    }
}

/* Initialize power rails from flash defaults (called from main after system init) */
void io_init_power_rails(void)
{
    bool pmic_en_default = true;
    bool wifi_en_default = true;
    bool wifi_buf_en_default = true;
    bool disp_en_default = false;
    
    /* Read power rail defaults from flash storage */
    int config_ret = read_power_rail_defaults(&pmic_en_default, &wifi_en_default, 
                                               &wifi_buf_en_default, &disp_en_default);
    
    /* CRITICAL: Configure power rail GPIOs using GPIO driver API
     * This MUST happen in the bootloader to ensure power rails are set before application starts */
    
    /* Get GPIOE device from device tree - use same pattern as LED initialization */
    const struct device *gpioe_dev = DEVICE_DT_GET(DT_NODELABEL(gpioe));
    
    /* Check if GPIO device is ready (same pattern as LED initialization)
     * If not ready, skip configuration - application will handle it */
    if (!device_is_ready(gpioe_dev)) {
        printk("W: GPIO E device not ready - skipping power rail configuration\n");
        printk("W: Application will configure power rails during initialization\n");
        /* Store states for logging even if configuration failed */
        power_rail_state.initialized = true;
        power_rail_state.pmic_en = pmic_en_default;
        power_rail_state.wifi_en = wifi_en_default;
        power_rail_state.wifi_buf_en = wifi_buf_en_default;
        power_rail_state.disp_en = disp_en_default;
        power_rail_state.from_flash = (config_ret == 0);
        return;
    }
    
    /* CRITICAL HARDWARE DEPENDENCY: WiFi pins must be configured HIGH FIRST
     * before PMIC can be driven HIGH. Hardware testing shows PMIC enable line
     * only goes HIGH when WiFi enable pins are HIGH. */
    
    int ret;
    
    /* Step 1: Configure WIFI_EN (PTE19) - MUST BE FIRST */
    ret = gpio_pin_configure(gpioe_dev, WIFI_EN_PIN, GPIO_OUTPUT | (wifi_en_default ? GPIO_OUTPUT_INIT_HIGH : GPIO_OUTPUT_INIT_LOW));
    if (ret < 0) {
        printk("E: Failed to configure WIFI_EN GPIO: %d\n", ret);
    }
    
    /* Step 2: Configure WIFI_BUF_EN (PTE18) - MUST BE SECOND */
    ret = gpio_pin_configure(gpioe_dev, WIFI_BUF_EN_PIN, GPIO_OUTPUT | (wifi_buf_en_default ? GPIO_OUTPUT_INIT_HIGH : GPIO_OUTPUT_INIT_LOW));
    if (ret < 0) {
        printk("E: Failed to configure WIFI_BUF_EN GPIO: %d\n", ret);
    }
    
    /* Step 3: Configure PMIC_EN (PTE17) - Can now be set HIGH if WiFi pins are HIGH */
    ret = gpio_pin_configure(gpioe_dev, PMIC_EN_PIN, GPIO_OUTPUT | (pmic_en_default ? GPIO_OUTPUT_INIT_HIGH : GPIO_OUTPUT_INIT_LOW));
    if (ret < 0) {
        printk("E: Failed to configure PMIC_EN GPIO: %d\n", ret);
    }
    
    /* Step 4: Configure DISP_EN (PTE16) */
    ret = gpio_pin_configure(gpioe_dev, DISP_EN_PIN, GPIO_OUTPUT | (disp_en_default ? GPIO_OUTPUT_INIT_HIGH : GPIO_OUTPUT_INIT_LOW));
    if (ret < 0) {
        printk("E: Failed to configure DISP_EN GPIO: %d\n", ret);
    }
    
    /* Store states for logging */
    power_rail_state.initialized = true;
    power_rail_state.pmic_en = pmic_en_default;
    power_rail_state.wifi_en = wifi_en_default;
    power_rail_state.wifi_buf_en = wifi_buf_en_default;
    power_rail_state.disp_en = disp_en_default;
    power_rail_state.from_flash = (config_ret == 0);
}

/* Check if PMIC is OFF (for bootloader decision making) */
bool power_rail_pmic_is_off(void)
{
    if (!power_rail_state.initialized) {
        return false;  /* Not initialized yet - assume ON */
    }
    return !power_rail_state.pmic_en;
}

/* Log power rail states after console is initialized */
void power_rail_log_status(void)
{
    if (!power_rail_state.initialized) {
        return;
    }
    
    /* Use printk for early boot messages - BOOT_LOG_INF may not be ready yet */
    printk("I: Power rail: PMIC_EN (PTE17) = %s\n", power_rail_state.pmic_en ? "ON" : "OFF");
    printk("I: Power rail: DISP_EN (PTE16) = %s\n", power_rail_state.disp_en ? "ON" : "OFF");
    printk("I: Power rail: WIFI_EN (PTE19) = %s\n", power_rail_state.wifi_en ? "ON" : "OFF");
    printk("I: Power rail: WIFI_BUF_EN (PTE18) = %s\n", power_rail_state.wifi_buf_en ? "ON" : "OFF");
    
    if (power_rail_state.from_flash) {
        printk("I: Power rails configured from flash storage\n");
    } else {
        printk("I: Power rails configured with default values\n");
    }
}
