/*
 * Minimal shared Arm CMN configuration-space model.
 *
 * The model only exposes a shared, RAM-backed CMN configuration aperture.
 * It seeds enough discovery-visible topology for SCP-firmware's cmn_cyprus
 * module to walk the mesh and program HN-S/RNSAM state. Functional CMN
 * behavior is intentionally out of scope for now.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/memory.h"

#include <sys/mman.h>
#include <unistd.h>

#define TYPE_ARM_CMN_CFG "arm-cmn-cfg"
OBJECT_DECLARE_SIMPLE_TYPE(ArmCmnCfgState, ARM_CMN_CFG)

#define ARM_CMN_CFG_DEFAULT_SIZE      0x40000000ULL
#define ARM_CMN_CFG_SEED_SIZE         0x00800000ULL

#define CMN_CFG_NODE_INFO             0x0000
#define CMN_CFG_PERIPH_ID_01          0x0008
#define CMN_CFG_PERIPH_ID_23          0x0010
#define CMN_CFG_PERIPH_ID_45          0x0018
#define CMN_CFG_PERIPH_ID_67          0x0020
#define CMN_CFG_COMPONENT_ID_01       0x0028
#define CMN_CFG_COMPONENT_ID_23       0x0030
#define CMN_CFG_CHILD_INFO            0x0080
#define CMN_CFG_CHILD_PTR_BASE        0x0100
#define CMN_CFG_INFO_GLOBAL           0x0900
#define CMN_CFG_INFO_GLOBAL_1         0x0908
#define CMN_CFG_SECURE_ACCESS         0x0980
#define CMN_CFG_RCR                   0x0988
#define CMN_CFG_SCR                   0x0990
#define CMN_CFG_RAS_MODE              0x0a00

#define CMN_NODE_CHILD_INFO           0x0080

#define CMN_MXP_PORT_INFO_BASE        0x0008
#define CMN_MXP_PORT_INFO_STEP        0x0008
#define CMN_MXP_CHILD_PTR_BASE        0x0100

#define CMN_NODE_TYPE_CFG_ROOT        0x0002
#define CMN_NODE_TYPE_HN_I            0x0004
#define CMN_NODE_TYPE_XP              0x0006
#define CMN_NODE_TYPE_SBSX            0x0007
#define CMN_NODE_TYPE_RN_I            0x000a
#define CMN_NODE_TYPE_RN_D            0x000d
#define CMN_NODE_TYPE_RN_SAM          0x000f
#define CMN_NODE_TYPE_HN_P            0x0011
#define CMN_NODE_TYPE_CCLA            0x0105
#define CMN_NODE_TYPE_HN_S            0x0200

#define CMN_DEVTYPE_RN_I              0x01
#define CMN_DEVTYPE_RN_D              0x02
#define CMN_DEVTYPE_HN_I              0x09
#define CMN_DEVTYPE_HN_D              0x0a
#define CMN_DEVTYPE_HN_P              0x0b
#define CMN_DEVTYPE_SBSX              0x0d
#define CMN_DEVTYPE_HN_S              0x1a
#define CMN_DEVTYPE_CCG               0x1e
#define CMN_DEVTYPE_RNF_ESAM          0x21
#define CMN_DEVTYPE_SNF               0x22

#define CMN_CHILD_COUNT_MASK          0xffffULL
#define CMN_CHILD_PTR_OFF_POS         16
#define CMN_NODE_INFO_TYPE_MASK       0xffffULL
#define CMN_NODE_INFO_ID_POS          16
#define CMN_NODE_INFO_LDID_POS        32
#define CMN_MXP_NUM_PORTS_POS         48

#define CMN_XP_STRIDE                 0x00010000U
#define CMN_XP_BASE_START             0x00010000U
#define CMN_NODE_STRIDE               0x00010000U
#define CMN_NODE_BASE_START           0x00100000U

typedef struct ArmCmnCfgState {
    SysBusDevice parent_obj;

    MemoryRegion cfg;
    char *shm_path;
    uint64_t cfg_size;
} ArmCmnCfgState;

struct CmnNodeSeed {
    uint16_t node_type;
    uint16_t node_id;
    uint16_t ldid;
};

struct CmnXpSeed {
    uint16_t node_id;
    uint16_t ldid;
    uint8_t port_count;
    uint8_t port_device_type[4];
    const struct CmnNodeSeed *children;
    size_t child_count;
};

static const struct CmnNodeSeed cmn_xp00_nodes[] = {
    { CMN_NODE_TYPE_SBSX, 0, 0 },
    { CMN_NODE_TYPE_HN_I, 2, 0 },
    { CMN_NODE_TYPE_RN_I, 4, 0 },
    { CMN_NODE_TYPE_CCLA, 6, 0 },
};

static const struct CmnNodeSeed cmn_xp01_nodes[] = {
    { CMN_NODE_TYPE_RN_SAM, 8, 0 },
    { CMN_NODE_TYPE_RN_SAM, 9, 1 },
    { CMN_NODE_TYPE_HN_S, 10, 0 },
    { CMN_NODE_TYPE_HN_S, 11, 1 },
    { CMN_NODE_TYPE_CCLA, 12, 1 },
};

static const struct CmnNodeSeed cmn_xp02_nodes[] = {
    { CMN_NODE_TYPE_SBSX, 16, 1 },
    { CMN_NODE_TYPE_SBSX, 17, 2 },
    { CMN_NODE_TYPE_SBSX, 18, 3 },
    { CMN_NODE_TYPE_SBSX, 19, 4 },
    { CMN_NODE_TYPE_HN_I, 20, 1 },
    { CMN_NODE_TYPE_RN_I, 22, 1 },
};

static const struct CmnNodeSeed cmn_xp10_nodes[] = {
    { CMN_NODE_TYPE_RN_I, 32, 2 },
    { CMN_NODE_TYPE_HN_P, 34, 0 },
    { CMN_NODE_TYPE_CCLA, 36, 2 },
};

static const struct CmnNodeSeed cmn_xp11_nodes[] = {
    { CMN_NODE_TYPE_RN_SAM, 40, 2 },
    { CMN_NODE_TYPE_RN_SAM, 41, 3 },
    { CMN_NODE_TYPE_HN_S, 44, 2 },
    { CMN_NODE_TYPE_HN_S, 45, 3 },
};

static const struct CmnNodeSeed cmn_xp12_nodes[] = {
    { CMN_NODE_TYPE_SBSX, 48, 5 },
    { CMN_NODE_TYPE_SBSX, 49, 6 },
    { CMN_NODE_TYPE_SBSX, 50, 7 },
    { CMN_NODE_TYPE_SBSX, 51, 8 },
    { CMN_NODE_TYPE_RN_I, 52, 3 },
};

static const struct CmnNodeSeed cmn_xp20_nodes[] = {
    { CMN_NODE_TYPE_RN_SAM, 64, 4 },
    { CMN_NODE_TYPE_RN_SAM, 65, 5 },
    { CMN_NODE_TYPE_HN_S, 66, 4 },
    { CMN_NODE_TYPE_HN_S, 67, 5 },
    { CMN_NODE_TYPE_HN_P, 68, 1 },
    { CMN_NODE_TYPE_CCLA, 70, 3 },
};

static const struct CmnNodeSeed cmn_xp21_nodes[] = {
    { CMN_NODE_TYPE_RN_SAM, 72, 6 },
    { CMN_NODE_TYPE_RN_SAM, 73, 7 },
    { CMN_NODE_TYPE_HN_S, 74, 6 },
    { CMN_NODE_TYPE_HN_S, 75, 7 },
    { CMN_NODE_TYPE_RN_I, 76, 4 },
};

static const struct CmnNodeSeed cmn_xp22_nodes[] = {
    { CMN_NODE_TYPE_SBSX, 80, 9 },
    { CMN_NODE_TYPE_SBSX, 81, 10 },
    { CMN_NODE_TYPE_SBSX, 82, 11 },
    { CMN_NODE_TYPE_SBSX, 83, 12 },
    { CMN_NODE_TYPE_RN_D, 84, 0 },
    { CMN_NODE_TYPE_HN_P, 86, 2 },
};

static const struct CmnXpSeed cmn_mesh[] = {
    {
        .node_id = 7,
        .ldid = 0,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_SBSX,
            CMN_DEVTYPE_HN_D,
            CMN_DEVTYPE_RN_I,
            CMN_DEVTYPE_CCG,
        },
        .children = cmn_xp00_nodes,
        .child_count = ARRAY_SIZE(cmn_xp00_nodes),
    },
    {
        .node_id = 15,
        .ldid = 1,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_RNF_ESAM,
            CMN_DEVTYPE_HN_S,
            CMN_DEVTYPE_CCG,
            0,
        },
        .children = cmn_xp01_nodes,
        .child_count = ARRAY_SIZE(cmn_xp01_nodes),
    },
    {
        .node_id = 23,
        .ldid = 2,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_SNF,
            CMN_DEVTYPE_SBSX,
            CMN_DEVTYPE_HN_I,
            CMN_DEVTYPE_RN_I,
        },
        .children = cmn_xp02_nodes,
        .child_count = ARRAY_SIZE(cmn_xp02_nodes),
    },
    {
        .node_id = 39,
        .ldid = 3,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_RN_I,
            CMN_DEVTYPE_HN_P,
            CMN_DEVTYPE_CCG,
            0,
        },
        .children = cmn_xp10_nodes,
        .child_count = ARRAY_SIZE(cmn_xp10_nodes),
    },
    {
        .node_id = 47,
        .ldid = 4,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_RNF_ESAM,
            0,
            CMN_DEVTYPE_HN_S,
            0,
        },
        .children = cmn_xp11_nodes,
        .child_count = ARRAY_SIZE(cmn_xp11_nodes),
    },
    {
        .node_id = 55,
        .ldid = 5,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_SNF,
            CMN_DEVTYPE_SBSX,
            CMN_DEVTYPE_RN_I,
            0,
        },
        .children = cmn_xp12_nodes,
        .child_count = ARRAY_SIZE(cmn_xp12_nodes),
    },
    {
        .node_id = 71,
        .ldid = 6,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_RNF_ESAM,
            CMN_DEVTYPE_HN_S,
            CMN_DEVTYPE_HN_P,
            CMN_DEVTYPE_CCG,
        },
        .children = cmn_xp20_nodes,
        .child_count = ARRAY_SIZE(cmn_xp20_nodes),
    },
    {
        .node_id = 79,
        .ldid = 7,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_RNF_ESAM,
            CMN_DEVTYPE_HN_S,
            CMN_DEVTYPE_RN_I,
            0,
        },
        .children = cmn_xp21_nodes,
        .child_count = ARRAY_SIZE(cmn_xp21_nodes),
    },
    {
        .node_id = 87,
        .ldid = 8,
        .port_count = 4,
        .port_device_type = {
            CMN_DEVTYPE_SNF,
            CMN_DEVTYPE_SBSX,
            CMN_DEVTYPE_RN_D,
            CMN_DEVTYPE_HN_P,
        },
        .children = cmn_xp22_nodes,
        .child_count = ARRAY_SIZE(cmn_xp22_nodes),
    },
};

static uint64_t cmn_make_node_info(uint16_t type, uint16_t id, uint16_t ldid)
{
    uint64_t value = 0;

    value |= type & CMN_NODE_INFO_TYPE_MASK;
    value |= (uint64_t)id << CMN_NODE_INFO_ID_POS;
    value |= (uint64_t)ldid << CMN_NODE_INFO_LDID_POS;

    return value;
}

static uint64_t cmn_make_xp_node_info(
    uint16_t id,
    uint16_t ldid,
    uint8_t port_count)
{
    return cmn_make_node_info(CMN_NODE_TYPE_XP, id, ldid) |
        ((uint64_t)port_count << CMN_MXP_NUM_PORTS_POS);
}

static uint64_t cmn_make_child_info(uint16_t count)
{
    return ((uint64_t)CMN_CFG_CHILD_PTR_BASE << CMN_CHILD_PTR_OFF_POS) |
        (count & CMN_CHILD_COUNT_MASK);
}

static void cmn_writeq(uint8_t *cfg, uint32_t offset, uint64_t value)
{
    stq_le_p(cfg + offset, value);
}

static void cmn_seed_root_registers(uint8_t *cfg)
{
    uint64_t info_global = 0;
    size_t i;

    cmn_writeq(cfg, CMN_CFG_NODE_INFO,
               cmn_make_node_info(CMN_NODE_TYPE_CFG_ROOT, 0, 0));

    /* PID/CID values taken from the CMN S3(AE) reset-state tables. */
    cmn_writeq(cfg, CMN_CFG_PERIPH_ID_01, 0x000000000000b43eULL);
    cmn_writeq(cfg, CMN_CFG_PERIPH_ID_23, 0x000000000000003bULL);
    cmn_writeq(cfg, CMN_CFG_PERIPH_ID_45, 0x00000000000000c4ULL);
    cmn_writeq(cfg, CMN_CFG_PERIPH_ID_67, 0x0000000000000000ULL);
    cmn_writeq(cfg, CMN_CFG_COMPONENT_ID_01, 0x000000000000f00dULL);
    cmn_writeq(cfg, CMN_CFG_COMPONENT_ID_23, 0x000000000000b105ULL);

    cmn_writeq(cfg, CMN_CFG_CHILD_INFO, cmn_make_child_info(ARRAY_SIZE(cmn_mesh)));

    for (i = 0; i < ARRAY_SIZE(cmn_mesh); ++i) {
        cmn_writeq(cfg,
                   CMN_CFG_CHILD_PTR_BASE + (i * sizeof(uint64_t)),
                   CMN_XP_BASE_START + (i * CMN_XP_STRIDE));
    }

    /*
     * Conservative discovery values:
     *   CHI version = CHI-D
     *   physical/REQ address width = 48 bits
     */
    info_global |= 4ULL << 56;
    info_global |= 48ULL << 16;
    info_global |= 48ULL << 8;
    cmn_writeq(cfg, CMN_CFG_INFO_GLOBAL, info_global);
    cmn_writeq(cfg, CMN_CFG_INFO_GLOBAL_1, 0);
    cmn_writeq(cfg, CMN_CFG_SECURE_ACCESS, 0);
    cmn_writeq(cfg, CMN_CFG_RCR, 0);
    cmn_writeq(cfg, CMN_CFG_SCR, 0);
    cmn_writeq(cfg, CMN_CFG_RAS_MODE, 0);
}

static void cmn_seed_mesh(uint8_t *cfg, uint64_t seed_size)
{
    uint32_t next_node_offset = CMN_NODE_BASE_START;
    size_t i, j;

    for (i = 0; i < ARRAY_SIZE(cmn_mesh); ++i) {
        const struct CmnXpSeed *xp = &cmn_mesh[i];
        uint32_t xp_offset = CMN_XP_BASE_START + (i * CMN_XP_STRIDE);

        g_assert((uint64_t)xp_offset + CMN_XP_STRIDE <= seed_size);

        cmn_writeq(cfg,
                   xp_offset + CMN_CFG_NODE_INFO,
                   cmn_make_xp_node_info(xp->node_id, xp->ldid,
                                         xp->port_count));
        cmn_writeq(cfg,
                   xp_offset + CMN_CFG_CHILD_INFO,
                   cmn_make_child_info(xp->child_count));

        for (j = 0; j < xp->port_count; ++j) {
            cmn_writeq(cfg,
                       xp_offset + CMN_MXP_PORT_INFO_BASE +
                           (j * CMN_MXP_PORT_INFO_STEP),
                       xp->port_device_type[j]);
        }

        for (j = 0; j < xp->child_count; ++j) {
            const struct CmnNodeSeed *node = &xp->children[j];
            uint32_t node_offset = next_node_offset;

            g_assert((uint64_t)node_offset + CMN_NODE_STRIDE <= seed_size);

            next_node_offset += CMN_NODE_STRIDE;

            cmn_writeq(cfg,
                       xp_offset + CMN_MXP_CHILD_PTR_BASE +
                           (j * sizeof(uint64_t)),
                       node_offset);

            cmn_writeq(cfg,
                       node_offset + CMN_CFG_NODE_INFO,
                       cmn_make_node_info(node->node_type,
                                          node->node_id,
                                          node->ldid));
            cmn_writeq(cfg, node_offset + CMN_NODE_CHILD_INFO, 0);
        }
    }
}

static void arm_cmn_cfg_seed_window(uint8_t *cfg, uint64_t seed_size)
{
    memset(cfg, 0, seed_size);
    cmn_seed_root_registers(cfg);
    cmn_seed_mesh(cfg, seed_size);
}

static bool arm_cmn_cfg_init_window(ArmCmnCfgState *s, Error **errp)
{
    void *ptr;
    int fd;
    bool ok;

    fd = open(s->shm_path, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        error_setg(errp, "%s: open(%s) failed: %s",
                   TYPE_ARM_CMN_CFG, s->shm_path, strerror(errno));
        return false;
    }

    if (ftruncate(fd, s->cfg_size) < 0) {
        error_setg(errp, "%s: ftruncate(%s) failed: %s",
                   TYPE_ARM_CMN_CFG, s->shm_path, strerror(errno));
        close(fd);
        return false;
    }

    ptr = mmap(NULL, ARM_CMN_CFG_SEED_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
               fd, 0);
    if (ptr == MAP_FAILED) {
        error_setg(errp, "%s: mmap(%s) failed: %s",
                   TYPE_ARM_CMN_CFG, s->shm_path, strerror(errno));
        close(fd);
        return false;
    }

    arm_cmn_cfg_seed_window(ptr, ARM_CMN_CFG_SEED_SIZE);
    munmap(ptr, ARM_CMN_CFG_SEED_SIZE);

    ok = memory_region_init_ram_from_fd(&s->cfg, OBJECT(s),
                                        TYPE_ARM_CMN_CFG ".cfg",
                                        s->cfg_size, RAM_SHARED, fd, 0,
                                        errp);
    close(fd);
    return ok;
}

static void arm_cmn_cfg_realize(DeviceState *dev, Error **errp)
{
    ArmCmnCfgState *s = ARM_CMN_CFG(dev);

    if (s->shm_path == NULL || s->cfg_size == 0) {
        error_setg(errp, "%s: shm-path and cfg-size must be set",
                   TYPE_ARM_CMN_CFG);
        return;
    }

    if (!arm_cmn_cfg_init_window(s, errp)) {
        return;
    }
}

static void arm_cmn_cfg_init(Object *obj)
{
    ArmCmnCfgState *s = ARM_CMN_CFG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    sysbus_init_mmio(sbd, &s->cfg);
}

static const VMStateDescription vmstate_arm_cmn_cfg = {
    .name = TYPE_ARM_CMN_CFG,
    .unmigratable = 1,
};

static const Property arm_cmn_cfg_properties[] = {
    DEFINE_PROP_STRING("shm-path", ArmCmnCfgState, shm_path),
    DEFINE_PROP_UINT64("cfg-size", ArmCmnCfgState, cfg_size,
                       ARM_CMN_CFG_DEFAULT_SIZE),
};

static void arm_cmn_cfg_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, arm_cmn_cfg_properties);
    dc->realize = arm_cmn_cfg_realize;
    dc->vmsd = &vmstate_arm_cmn_cfg;
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo arm_cmn_cfg_info = {
    .name = TYPE_ARM_CMN_CFG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArmCmnCfgState),
    .instance_init = arm_cmn_cfg_init,
    .class_init = arm_cmn_cfg_class_init,
};

static void arm_cmn_cfg_register_types(void)
{
    type_register_static(&arm_cmn_cfg_info);
}

type_init(arm_cmn_cfg_register_types)
