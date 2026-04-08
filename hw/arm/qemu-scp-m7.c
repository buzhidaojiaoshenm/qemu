/*
 * Custom QEMU SCP board for qemu_virt_m7 firmware.
 *
 * This board keeps a Cortex-M7 CPU but exposes a PL011 UART and a
 * minimal MMIO generic-timer frame tailored for SCP-firmware's pl011,
 * gtimer and timer modules.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/pl011.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "qom/object.h"

#define TYPE_QEMU_SCP_M7_MACHINE MACHINE_TYPE_NAME("qemu-scp-m7")
#define TYPE_QEMU_SCP_GTIMER "qemu-scp-gtimer"

#define QEMU_SCP_M7_SYSCLK_HZ          25000000
#define QEMU_SCP_M7_REFCLK_HZ          1000000
#define QEMU_SCP_M7_ITCM_BASE          0x00000000
#define QEMU_SCP_M7_ITCM_SIZE          0x000c0000
#define QEMU_SCP_M7_DTCM_BASE          0x20000000
#define QEMU_SCP_M7_DTCM_SIZE          0x00080000
#define QEMU_SCP_M7_RAM_SIZE           (QEMU_SCP_M7_ITCM_SIZE + QEMU_SCP_M7_DTCM_SIZE)
#define QEMU_SCP_M7_GTIMER_CNTCTL_BASE 0x44000000
#define QEMU_SCP_M7_GTIMER_CTRL_BASE   0x44000800
#define QEMU_SCP_M7_GTIMER_BASE        0x44001000
#define QEMU_SCP_M7_PL011_BASE         0x44002000
#define QEMU_SCP_M7_GTIMER_IRQ         8

#define SCMI_BRIDGE_TYPE               "scmi-mailbox-bridge"
#define SCMI_BRIDGE_SCP_MMIO_BASE      0x45000000
#define SCMI_BRIDGE_SCP_SHM_BASE       0x45010000
#define SCMI_BRIDGE_SCP_SHM_SIZE       0x00010000
#define SCMI_BRIDGE_SCP_IRQ            11
#define SCMI_BRIDGE_SHM_PATH           "/tmp/qemu_virt_soc.scmi_bridge.shm"
#define SCMI_BRIDGE_AP_SOCK_PATH       "/tmp/qemu_virt_soc.ap.sock"
#define SCMI_BRIDGE_SCP_SOCK_PATH      "/tmp/qemu_virt_soc.scp.sock"

#define QEMU_SCP_M7_MHU_S_RCV_BASE     0x45020000
#define QEMU_SCP_M7_MHU_S_SND_BASE     0x45030000
#define QEMU_SCP_M7_MHU_WINDOW_SIZE    0x00010000

#define GTIMER_CNTBASE_REGION_SIZE     0x1000
#define GTIMER_CNTCTL_REGION_SIZE      0x0100
#define GTIMER_CNTCONTROL_REGION_SIZE  0x0100

#define CNTBASE_P_CTL_ENABLE           0x00000001
#define CNTBASE_P_CTL_IMASK            0x00000002
#define CNTBASE_P_CTL_ISTATUS          0x00000004

#define CNTCONTROL_CR_EN               0x00000001
#define CNTCONTROL_CR_FCREQ            0x00000100

typedef struct QemuScpGTimerState {
    SysBusDevice parent_obj;

    MemoryRegion cntbase_iomem;
    MemoryRegion cntctl_iomem;
    MemoryRegion cntcontrol_iomem;
    qemu_irq irq;
    QEMUTimer *compare_timer;

    uint32_t frequency;
    uint32_t timer_ctl;
    uint32_t acr;
    uint32_t control_cr;
    uint32_t control_fid0;
    uint64_t compare_value;
    uint64_t counter_offset;
    int64_t counter_base_ns;
    bool irq_pending;
} QemuScpGTimerState;

typedef struct QemuScpM7MachineState {
    MachineState parent;

    ARMv7MState armv7m;
    MemoryRegion itcm;
    MemoryRegion dtcm;
    Clock *sysclk;
    Clock *refclk;
} QemuScpM7MachineState;

OBJECT_DECLARE_SIMPLE_TYPE(QemuScpGTimerState, QEMU_SCP_GTIMER)
OBJECT_DECLARE_SIMPLE_TYPE(QemuScpM7MachineState, QEMU_SCP_M7_MACHINE)

static uint64_t qemu_scp_gtimer_current_counter(QemuScpGTimerState *s)
{
    int64_t now_ns;
    uint64_t elapsed_ticks;

    if ((s->control_cr & CNTCONTROL_CR_EN) == 0) {
        return s->counter_offset;
    }

    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now_ns <= s->counter_base_ns) {
        return s->counter_offset;
    }

    elapsed_ticks = muldiv64(
        (uint64_t)(now_ns - s->counter_base_ns), s->frequency, NANOSECONDS_PER_SECOND);

    return s->counter_offset + elapsed_ticks;
}

static void qemu_scp_gtimer_raise_irq(QemuScpGTimerState *s, bool level)
{
    qemu_set_irq(s->irq, level);
}

static void qemu_scp_gtimer_reschedule(QemuScpGTimerState *s)
{
    uint64_t counter;
    uint64_t delta_ticks;
    uint64_t delta_ns;
    bool enabled;

    enabled = ((s->control_cr & CNTCONTROL_CR_EN) != 0) &&
        ((s->timer_ctl & CNTBASE_P_CTL_ENABLE) != 0);

    timer_del(s->compare_timer);

    if (!enabled) {
        s->irq_pending = false;
        qemu_scp_gtimer_raise_irq(s, false);
        return;
    }

    counter = qemu_scp_gtimer_current_counter(s);
    if (counter >= s->compare_value) {
        s->irq_pending = true;
        qemu_scp_gtimer_raise_irq(s, (s->timer_ctl & CNTBASE_P_CTL_IMASK) == 0);
        return;
    }

    s->irq_pending = false;
    qemu_scp_gtimer_raise_irq(s, false);

    delta_ticks = s->compare_value - counter;
    delta_ns = muldiv64(delta_ticks, NANOSECONDS_PER_SECOND, s->frequency);
    timer_mod(s->compare_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delta_ns);
}

static void qemu_scp_gtimer_compare_expired(void *opaque)
{
    QemuScpGTimerState *s = opaque;

    s->irq_pending = true;
    qemu_scp_gtimer_raise_irq(s, (s->timer_ctl & CNTBASE_P_CTL_IMASK) == 0);
}

static uint64_t qemu_scp_gtimer_cntbase_read(
    void *opaque,
    hwaddr offset,
    unsigned size)
{
    QemuScpGTimerState *s = opaque;
    uint64_t counter;

    counter = qemu_scp_gtimer_current_counter(s);

    switch (offset) {
    case 0x000:
        return (uint32_t)counter;
    case 0x004:
        return (uint32_t)(counter >> 32);
    case 0x010:
        return s->frequency;
    case 0x020:
        return (uint32_t)s->compare_value;
    case 0x024:
        return (uint32_t)(s->compare_value >> 32);
    case 0x02c:
        return s->timer_ctl |
            (s->irq_pending ? CNTBASE_P_CTL_ISTATUS : 0);
    default:
        return 0;
    }
}

static void qemu_scp_gtimer_cntbase_write(
    void *opaque,
    hwaddr offset,
    uint64_t value,
    unsigned size)
{
    QemuScpGTimerState *s = opaque;

    switch (offset) {
    case 0x020:
        s->compare_value &= 0xffffffff00000000ULL;
        s->compare_value |= (uint32_t)value;
        break;
    case 0x024:
        s->compare_value &= 0x00000000ffffffffULL;
        s->compare_value |= ((uint64_t)(uint32_t)value) << 32;
        break;
    case 0x028:
        s->compare_value = qemu_scp_gtimer_current_counter(s) + (uint32_t)value;
        break;
    case 0x02c:
        s->timer_ctl = (uint32_t)value & (
            CNTBASE_P_CTL_ENABLE | CNTBASE_P_CTL_IMASK);
        if ((s->timer_ctl & CNTBASE_P_CTL_ENABLE) == 0) {
            s->irq_pending = false;
        }
        break;
    default:
        return;
    }

    qemu_scp_gtimer_reschedule(s);
}

static const MemoryRegionOps qemu_scp_gtimer_cntbase_ops = {
    .read = qemu_scp_gtimer_cntbase_read,
    .write = qemu_scp_gtimer_cntbase_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t qemu_scp_gtimer_cntctl_read(
    void *opaque,
    hwaddr offset,
    unsigned size)
{
    QemuScpGTimerState *s = opaque;

    switch (offset) {
    case 0x000:
        return s->frequency;
    case 0x040:
        return s->acr;
    default:
        return 0;
    }
}

static void qemu_scp_gtimer_cntctl_write(
    void *opaque,
    hwaddr offset,
    uint64_t value,
    unsigned size)
{
    QemuScpGTimerState *s = opaque;

    switch (offset) {
    case 0x000:
        if ((uint32_t)value != 0) {
            s->frequency = (uint32_t)value;
            s->control_fid0 = (uint32_t)value;
        }
        break;
    case 0x040:
        s->acr = (uint32_t)value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps qemu_scp_gtimer_cntctl_ops = {
    .read = qemu_scp_gtimer_cntctl_read,
    .write = qemu_scp_gtimer_cntctl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t qemu_scp_gtimer_cntcontrol_read(
    void *opaque,
    hwaddr offset,
    unsigned size)
{
    QemuScpGTimerState *s = opaque;
    uint64_t counter;

    counter = qemu_scp_gtimer_current_counter(s);

    switch (offset) {
    case 0x000:
        return s->control_cr;
    case 0x004:
        return ((s->control_cr & CNTCONTROL_CR_EN) != 0) ? 1 : 0;
    case 0x008:
        return (uint32_t)counter;
    case 0x00c:
        return (uint32_t)(counter >> 32);
    case 0x020:
        return s->control_fid0;
    default:
        return 0;
    }
}

static void qemu_scp_gtimer_cntcontrol_write(
    void *opaque,
    hwaddr offset,
    uint64_t value,
    unsigned size)
{
    QemuScpGTimerState *s = opaque;
    uint32_t new_cr;
    bool was_enabled;
    bool now_enabled;

    switch (offset) {
    case 0x000:
        was_enabled = (s->control_cr & CNTCONTROL_CR_EN) != 0;
        now_enabled = (((uint32_t)value) & CNTCONTROL_CR_EN) != 0;
        if (was_enabled && !now_enabled) {
            s->counter_offset = qemu_scp_gtimer_current_counter(s);
        } else if (!was_enabled && now_enabled) {
            s->counter_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }

        new_cr = (uint32_t)value;
        s->control_cr = new_cr & (CNTCONTROL_CR_EN | CNTCONTROL_CR_FCREQ);
        qemu_scp_gtimer_reschedule(s);
        break;
    case 0x020:
        if ((uint32_t)value != 0) {
            s->control_fid0 = (uint32_t)value;
            s->frequency = (uint32_t)value;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps qemu_scp_gtimer_cntcontrol_ops = {
    .read = qemu_scp_gtimer_cntcontrol_read,
    .write = qemu_scp_gtimer_cntcontrol_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void qemu_scp_gtimer_reset(DeviceState *dev)
{
    QemuScpGTimerState *s = QEMU_SCP_GTIMER(dev);

    timer_del(s->compare_timer);
    s->frequency = QEMU_SCP_M7_SYSCLK_HZ;
    s->control_fid0 = QEMU_SCP_M7_SYSCLK_HZ;
    s->timer_ctl = CNTBASE_P_CTL_IMASK;
    s->acr = 0;
    s->control_cr = 0;
    s->compare_value = 0;
    s->counter_offset = 0;
    s->counter_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->irq_pending = false;
    qemu_scp_gtimer_raise_irq(s, false);
}

static void qemu_scp_gtimer_init(Object *obj)
{
    QemuScpGTimerState *s = QEMU_SCP_GTIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(
        &s->cntbase_iomem, obj, &qemu_scp_gtimer_cntbase_ops, s,
        "qemu-scp-gtimer-cntbase", GTIMER_CNTBASE_REGION_SIZE);
    memory_region_init_io(
        &s->cntctl_iomem, obj, &qemu_scp_gtimer_cntctl_ops, s,
        "qemu-scp-gtimer-cntctl", GTIMER_CNTCTL_REGION_SIZE);
    memory_region_init_io(
        &s->cntcontrol_iomem, obj, &qemu_scp_gtimer_cntcontrol_ops, s,
        "qemu-scp-gtimer-cntcontrol", GTIMER_CNTCONTROL_REGION_SIZE);

    sysbus_init_mmio(sbd, &s->cntbase_iomem);
    sysbus_init_mmio(sbd, &s->cntctl_iomem);
    sysbus_init_mmio(sbd, &s->cntcontrol_iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->compare_timer = timer_new_ns(
        QEMU_CLOCK_VIRTUAL, qemu_scp_gtimer_compare_expired, s);
}

static void qemu_scp_gtimer_finalize(Object *obj)
{
    QemuScpGTimerState *s = QEMU_SCP_GTIMER(obj);

    timer_free(s->compare_timer);
}

static void qemu_scp_m7_create_uart(DeviceState *armv7m)
{
    DeviceState *dev = qdev_new(TYPE_PL011);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, QEMU_SCP_M7_PL011_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, 1));
}

static void qemu_scp_m7_create_gtimer(DeviceState *armv7m)
{
    DeviceState *dev = qdev_new(TYPE_QEMU_SCP_GTIMER);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, QEMU_SCP_M7_GTIMER_BASE);
    sysbus_mmio_map(sbd, 1, QEMU_SCP_M7_GTIMER_CNTCTL_BASE);
    sysbus_mmio_map(sbd, 2, QEMU_SCP_M7_GTIMER_CTRL_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, QEMU_SCP_M7_GTIMER_IRQ));
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
        &sms->itcm, NULL, "qemu-scp-m7.itcm", QEMU_SCP_M7_ITCM_SIZE,
        &error_fatal);
    memory_region_add_subregion(
        get_system_memory(), QEMU_SCP_M7_ITCM_BASE, &sms->itcm);

    memory_region_init_ram(
        &sms->dtcm, NULL, "qemu-scp-m7.dtcm", QEMU_SCP_M7_DTCM_SIZE,
        &error_fatal);
    memory_region_add_subregion(
        get_system_memory(), QEMU_SCP_M7_DTCM_BASE, &sms->dtcm);

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
        "qemu-scp-m7.timer-cfg-gap0", 0x44000100, 0x00000700);
    create_unimplemented_device(
        "qemu-scp-m7.timer-cfg-gap1", 0x44000900, 0x00000700);
    create_unimplemented_device(
        "qemu-scp-m7.uart-cfg-reserved", 0x44003000, 0x00002000);
    create_unimplemented_device(
        "qemu-scp-m7.mhu-secure-rcv", QEMU_SCP_M7_MHU_S_RCV_BASE,
        QEMU_SCP_M7_MHU_WINDOW_SIZE);
    create_unimplemented_device(
        "qemu-scp-m7.mhu-secure-snd", QEMU_SCP_M7_MHU_S_SND_BASE,
        QEMU_SCP_M7_MHU_WINDOW_SIZE);

    qemu_scp_m7_create_gtimer(armv7m);
    qemu_scp_m7_create_uart(armv7m);
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

static void qemu_scp_gtimer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, qemu_scp_gtimer_reset);
}

static const TypeInfo qemu_scp_gtimer_info = {
    .name = TYPE_QEMU_SCP_GTIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QemuScpGTimerState),
    .instance_init = qemu_scp_gtimer_init,
    .instance_finalize = qemu_scp_gtimer_finalize,
    .class_init = qemu_scp_gtimer_class_init,
};

static const TypeInfo qemu_scp_m7_machine_info = {
    .name = TYPE_QEMU_SCP_M7_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(QemuScpM7MachineState),
    .class_init = qemu_scp_m7_machine_class_init,
    .interfaces = arm_machine_interfaces,
};

static void qemu_scp_m7_register_types(void)
{
    type_register_static(&qemu_scp_gtimer_info);
    type_register_static(&qemu_scp_m7_machine_info);
}

type_init(qemu_scp_m7_register_types);
