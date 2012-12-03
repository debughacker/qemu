/*
 * Xilinx Zynq Baseboard System emulation.
 *
 * Copyright (c) 2010 Xilinx.
 * Copyright (c) 2012 Peter A.G. Crosthwaite (peter.croshtwaite@petalogix.com)
 * Copyright (c) 2012 Petalogix Pty Ltd.
 * Written by Haibing Ma
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "sysbus.h"
#include "arm-misc.h"
#include "net.h"
#include "exec-memory.h"
#include "sysemu.h"
#include "boards.h"
#include "flash.h"
#include "blockdev.h"
#include "loader.h"
#include "ssi.h"
#include "i2c.h"

#define NUM_SPI_FLASHES 4
#define NUM_QSPI_FLASHES 2
#define NUM_QSPI_BUSSES 2

#define FLASH_SIZE (64 * 1024 * 1024)
#define FLASH_SECTOR_SIZE (128 * 1024)

#define NUM_I2C_EEPROMS 2

#define IRQ_OFFSET 32 /* pic interrupts start from index 32 */

static struct arm_boot_info zynq_binfo = {};

static void gem_init(NICInfo *nd, uint32_t base, qemu_irq irq)
{
    DeviceState *dev;
    SysBusDevice *s;

    qemu_check_nic_model(nd, "cadence_gem");
    dev = qdev_create(NULL, "cadence_gem");
    qdev_set_nic_properties(dev, nd);
    qdev_init_nofail(dev);
    s = sysbus_from_qdev(dev);
    sysbus_mmio_map(s, 0, base);
    sysbus_connect_irq(s, 0, irq);
}

static inline void zynq_init_spi_flashes(uint32_t base_addr, qemu_irq irq,
                                         bool is_qspi)
{
    DeviceState *dev;
    SysBusDevice *busdev;
    SSIBus *spi;
    int i, j;
    int num_busses =  is_qspi ? NUM_QSPI_BUSSES : 1;
    int num_ss = is_qspi ? NUM_QSPI_FLASHES : NUM_SPI_FLASHES;

    dev = qdev_create(NULL, "xilinx,spips");
    qdev_prop_set_uint8(dev, "num-txrx-bytes", is_qspi ? 4 : 1);
    qdev_prop_set_uint8(dev, "num-ss-bits", num_ss);
    qdev_prop_set_uint8(dev, "num-busses", num_busses);
    qdev_init_nofail(dev);
    busdev = sysbus_from_qdev(dev);
    sysbus_mmio_map(busdev, 0, base_addr);
    if (is_qspi) {
        sysbus_mmio_map(busdev, 1, 0xFC000000);
    }
    sysbus_connect_irq(busdev, 0, irq);

    for (i = 0; i < num_busses; ++i) {
        char bus_name[16];
        qemu_irq cs_line;

        snprintf(bus_name, 16, "spi%d", i);
        spi = (SSIBus *)qdev_get_child_bus(dev, bus_name);

        for (j = 0; j < num_ss; ++j) {
            dev = ssi_create_slave_no_init(spi, "m25p80");
            qdev_prop_set_string(dev, "partname", "n25q128");
            qdev_init_nofail(dev);

            cs_line = qdev_get_gpio_in(dev, 0);
            sysbus_connect_irq(busdev, i * num_ss + j + 1, cs_line);
        }
    }

}

static inline void zynq_init_zc70x_i2c(uint32_t base_addr, qemu_irq irq)
{
    DeviceState *dev = sysbus_create_simple("xlnx.ps7-i2c", base_addr, irq);
    i2c_bus *i2c = (i2c_bus *)qdev_get_child_bus(dev, "i2c");
    int i, bus;

    dev = i2c_create_slave(i2c, "pca9548", 0);
    for (bus = 2; bus <= 3; bus++) {
        char bus_name[16];

        snprintf(bus_name, sizeof(bus_name), "i2c@%d", bus);
        i2c = (i2c_bus *)qdev_get_child_bus(dev, bus_name);
        assert(i2c);

        assert(NUM_I2C_EEPROMS <= 2); /* not enough address space for anymore */
        for (i = 0; i < NUM_I2C_EEPROMS; ++i) {
            DeviceState *eeprom_dev = i2c_create_slave_no_init(i2c, "at.24c08",
                                                               0x50 + 0x4 * i);
            qdev_prop_set_uint16(eeprom_dev, "size", 1024); /* M24C08 */
            qdev_init_nofail(eeprom_dev);
        }
    }
}

static void zynq_init(QEMUMachineInitArgs *args)
{
    ram_addr_t ram_size = args->ram_size;
    const char *cpu_model = args->cpu_model;
    const char *kernel_filename = args->kernel_filename;
    const char *kernel_cmdline = args->kernel_cmdline;
    const char *initrd_filename = args->initrd_filename;
    ARMCPU *cpu;
    MemoryRegion *address_space_mem = get_system_memory();
    MemoryRegion *ext_ram = g_new(MemoryRegion, 1);
    MemoryRegion *ocm_ram = g_new(MemoryRegion, 1);
    DeviceState *dev;
    SysBusDevice *busdev;
    qemu_irq *irqp;
    qemu_irq pic[64];
    NICInfo *nd;
    int n;
    qemu_irq cpu_irq;

    if (!cpu_model) {
        cpu_model = "cortex-a9";
    }

    cpu = cpu_arm_init(cpu_model);
    if (!cpu) {
        fprintf(stderr, "Unable to find CPU definition\n");
        exit(1);
    }
    irqp = arm_pic_init_cpu(cpu);
    cpu_irq = irqp[ARM_PIC_CPU_IRQ];

    /* max 2GB ram */
    if (ram_size > 0x80000000) {
        ram_size = 0x80000000;
    }

    /* DDR remapped to address zero.  */
    memory_region_init_ram(ext_ram, "zynq.ext_ram", ram_size);
    vmstate_register_ram_global(ext_ram);
    memory_region_add_subregion(address_space_mem, 0, ext_ram);

    /* 256K of on-chip memory */
    memory_region_init_ram(ocm_ram, "zynq.ocm_ram", 256 << 10);
    vmstate_register_ram_global(ocm_ram);
    memory_region_add_subregion(address_space_mem, 0xFFFC0000, ocm_ram);

    DriveInfo *dinfo = drive_get(IF_PFLASH, 0, 0);

    /* AMD */
    pflash_cfi02_register(0xe2000000, NULL, "zynq.pflash", FLASH_SIZE,
                          dinfo ? dinfo->bdrv : NULL, FLASH_SECTOR_SIZE,
                          FLASH_SIZE/FLASH_SECTOR_SIZE, 1,
                          1, 0x0066, 0x0022, 0x0000, 0x0000, 0x0555, 0x2aa,
                              0);

    dev = qdev_create(NULL, "xilinx,zynq_slcr");
    qdev_init_nofail(dev);
    sysbus_mmio_map(sysbus_from_qdev(dev), 0, 0xF8000000);

    dev = qdev_create(NULL, "a9mpcore_priv");
    qdev_prop_set_uint32(dev, "num-cpu", 1);
    qdev_init_nofail(dev);
    busdev = sysbus_from_qdev(dev);
    sysbus_mmio_map(busdev, 0, 0xF8F00000);
    sysbus_connect_irq(busdev, 0, cpu_irq);

    for (n = 0; n < 64; n++) {
        pic[n] = qdev_get_gpio_in(dev, n);
    }

    zynq_init_zc70x_i2c(0xE0004000, pic[57-IRQ_OFFSET]);
    zynq_init_zc70x_i2c(0xE0005000, pic[80-IRQ_OFFSET]);

    zynq_init_spi_flashes(0xE0006000, pic[58-IRQ_OFFSET], false);
    zynq_init_spi_flashes(0xE0007000, pic[81-IRQ_OFFSET], false);
    zynq_init_spi_flashes(0xE000D000, pic[51-IRQ_OFFSET], true);

    sysbus_create_simple("xlnx,ps7-usb", 0xE0002000, pic[53-IRQ_OFFSET]);
    sysbus_create_simple("xlnx,ps7-usb", 0xE0003000, pic[75-IRQ_OFFSET]);

    sysbus_create_simple("cadence_uart", 0xE0000000, pic[59-IRQ_OFFSET]);
    sysbus_create_simple("cadence_uart", 0xE0001000, pic[82-IRQ_OFFSET]);

    sysbus_create_varargs("cadence_ttc", 0xF8001000,
            pic[42-IRQ_OFFSET], pic[43-IRQ_OFFSET], pic[44-IRQ_OFFSET], NULL);
    sysbus_create_varargs("cadence_ttc", 0xF8002000,
            pic[69-IRQ_OFFSET], pic[70-IRQ_OFFSET], pic[71-IRQ_OFFSET], NULL);

    for (n = 0; n < nb_nics; n++) {
        nd = &nd_table[n];
        if (n == 0) {
            gem_init(nd, 0xE000B000, pic[54-IRQ_OFFSET]);
        } else if (n == 1) {
            gem_init(nd, 0xE000C000, pic[77-IRQ_OFFSET]);
        }
    }

    zynq_binfo.ram_size = ram_size;
    zynq_binfo.kernel_filename = kernel_filename;
    zynq_binfo.kernel_cmdline = kernel_cmdline;
    zynq_binfo.initrd_filename = initrd_filename;
    zynq_binfo.nb_cpus = 1;
    zynq_binfo.board_id = 0xd32;
    zynq_binfo.loader_start = 0;
    arm_load_kernel(arm_env_get_cpu(first_cpu), &zynq_binfo);
}

static QEMUMachine zynq_machine = {
    .name = "xilinx-zynq-a9",
    .desc = "Xilinx Zynq Platform Baseboard for Cortex-A9",
    .init = zynq_init,
    .use_scsi = 1,
    .max_cpus = 1,
    .no_sdcard = 1
};

static void zynq_machine_init(void)
{
    qemu_register_machine(&zynq_machine);
}

machine_init(zynq_machine_init);
