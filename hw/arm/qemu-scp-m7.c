/*
 * Minimal QEMU SCP board for qemu_virt_m7 firmware.
 *
 * This is a custom Cortex-M7 board used by the qemu_virt_soc project. It
 * intentionally provides only the devices that the current SCP firmware needs:
 * RAM, one CMSDK UART, two CMSDK timers, and the SCMI mailbox bridge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/arm/boot.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/cmsdk-apb-uart.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "hw/timer/cmsdk-apb-timer.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "qom/object.h"

#define TYPE_QEMU_SCP_M7_MACHINE MACHINE_TYPE_NAME("qemu-scp-m7")

#define QEMU_SCP_M7_SYSCLK_HZ       25000000
#define QEMU_SCP_M7_REFCLK_HZ       1000000
#define QEMU_SCP_M7_RAM_BASE        0x00000000
#define QEMU_SCP_M7_RAM_SIZE        0x00400000
#define QEMU_SCP_M7_UART0_BASE      0x40004000
#define QEMU_SCP_M7_TIMER0_BASE     0x40000000
#define QEMU_SCP_M7_TIMER1_BASE     0x40001000
#define QEMU_SCP_M7_TIMER0_IRQ      8
#define QEMU_SCP_M7_TIMER1_IRQ      9

#define SCMI_BRIDGE_TYPE            "scmi-mailbox-bridge"
#define SCMI_BRIDGE_SCP_MMIO_BASE   0x40014000
#define SCMI_BRIDGE_SCP_SHM_BASE    0x40015000
#define SCMI_BRIDGE_SCP_SHM_SIZE    0x00001000
#define SCMI_BRIDGE_SCP_IRQ         11
#define SCMI_BRIDGE_SHM_PATH        "/tmp/qemu_virt_soc.scmi_bridge.shm"
#define SCMI_BRIDGE_AP_SOCK_PATH    "/tmp/qemu_virt_soc.ap.sock"
#define SCMI_BRIDGE_SCP_SOCK_PATH   "/tmp/qemu_virt_soc.scp.sock"

typedef struct QemuScpM7MachineState {
    MachineState parent;

    ARMv7MState armv7m;
    MemoryRegion ram;
    CMSDKAPBTimer timer[2];
    Clock *sysclk;
    Clock *refclk;
} QemuScpM7MachineState;

OBJECT_DECLARE_SIMPLE_TYPE(QemuScpM7MachineState, QEMU_SCP_M7_MACHINE)

static void qemu_scp_m7_create_uart(DeviceState *armv7m)
{
    DeviceState *dev = qdev_new(TYPE_CMSDK_APB_UART);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    qdev_prop_set_uint32(dev, "pclk-frq", QEMU_SCP_M7_SYSCLK_HZ);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, QEMU_SCP_M7_UART0_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, 1));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(armv7m, 0));
}

static void qemu_scp_m7_create_timer(
    QemuScpM7MachineState *sms,
    unsigned int index,
    hwaddr base,
    unsigned int irq)
{
    g_autofree char *name = g_strdup_printf("timer%u", index);

    object_initialize_child(
        OBJECT(sms), name, &sms->timer[index], TYPE_CMSDK_APB_TIMER);
    qdev_connect_clock_in(DEVICE(&sms->timer[index]), "pclk", sms->sysclk);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(&sms->timer[index]), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&sms->timer[index]), 0, base);
    sysbus_connect_irq(
        SYS_BUS_DEVICE(&sms->timer[index]), 0, qdev_get_gpio_in(
                                                  DEVICE(&sms->armv7m), irq));
}

static void qemu_scp_m7_create_scmi_bridge(DeviceState *armv7m)
{
    DeviceState *dev = qdev_new(SCMI_BRIDGE_TYPE);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    qdev_prop_set_string(dev, "local-socket-path", SCMI_BRIDGE_SCP_SOCK_PATH);
    qdev_prop_set_string(dev, "peer-socket-path", SCMI_BRIDGE_AP_SOCK_PATH);
    qdev_prop_set_string(dev, "shm-path", SCMI_BRIDGE_SHM_PATH);
    qdev_prop_set_uint64(dev, "shm-size", SCMI_BRIDGE_SCP_SHM_SIZE);

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, SCMI_BRIDGE_SCP_MMIO_BASE);
    sysbus_mmio_map(sbd, 1, SCMI_BRIDGE_SCP_SHM_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, SCMI_BRIDGE_SCP_IRQ));
}

static void qemu_scp_m7_init(MachineState *machine)
{
    QemuScpM7MachineState *sms = QEMU_SCP_M7_MACHINE(machine);
    DeviceState *armv7m;
    MachineClass *mc = MACHINE_GET_CLASS(machine);

    if (machine->ram_size != mc->default_ram_size) {
        char *sz = size_to_str(mc->default_ram_size);

        error_report("Invalid RAM size, should be %s", sz);
        g_free(sz);
        exit(EXIT_FAILURE);
    }

    sms->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sms->sysclk, QEMU_SCP_M7_SYSCLK_HZ);

    sms->refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(sms->refclk, QEMU_SCP_M7_REFCLK_HZ);

    memory_region_init_ram(
        &sms->ram,
        NULL,
        "qemu-scp-m7.ram",
        machine->ram_size,
        &error_fatal);
    memory_region_add_subregion(
        get_system_memory(), QEMU_SCP_M7_RAM_BASE, &sms->ram);

    object_initialize_child(OBJECT(sms), "armv7m", &sms->armv7m, TYPE_ARMV7M);
    armv7m = DEVICE(&sms->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", 32);
    qdev_prop_set_uint32(armv7m, "mpu-ns-regions", 16);
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    qdev_connect_clock_in(armv7m, "cpuclk", sms->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", sms->refclk);
    object_property_set_link(
        OBJECT(&sms->armv7m), "memory", OBJECT(get_system_memory()),
        &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&sms->armv7m), &error_fatal);

    create_unimplemented_device(
        "qemu-scp-m7.apb", 0x40000000, 0x00010000);

    qemu_scp_m7_create_uart(armv7m);
    qemu_scp_m7_create_timer(
        sms, 0, QEMU_SCP_M7_TIMER0_BASE, QEMU_SCP_M7_TIMER0_IRQ);
    qemu_scp_m7_create_timer(
        sms, 1, QEMU_SCP_M7_TIMER1_BASE, QEMU_SCP_M7_TIMER1_IRQ);
    qemu_scp_m7_create_scmi_bridge(armv7m);

    armv7m_load_kernel(
        sms->armv7m.cpu, machine->kernel_filename, 0, machine->ram_size);
}

static void qemu_scp_m7_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char *const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m7"),
        NULL,
    };

    mc->desc = "Custom SCP board for qemu_virt_m7 (Cortex-M7)";
    mc->init = qemu_scp_m7_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m7");
    mc->default_ram_size = QEMU_SCP_M7_RAM_SIZE;
    mc->default_ram_id = "scp.ram";
    mc->valid_cpu_types = valid_cpu_types;
}

static const TypeInfo qemu_scp_m7_machine_info = {
    .name = TYPE_QEMU_SCP_M7_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(QemuScpM7MachineState),
    .class_init = qemu_scp_m7_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

static void qemu_scp_m7_machine_register_types(void)
{
    type_register_static(&qemu_scp_m7_machine_info);
}

type_init(qemu_scp_m7_machine_register_types);
