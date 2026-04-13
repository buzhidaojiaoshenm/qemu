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
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "system/memory.h"

#include <sys/mman.h>
#include <unistd.h>

#define TYPE_ARM_CMN_CFG "arm-cmn-cfg"
OBJECT_DECLARE_SIMPLE_TYPE(ArmCmnCfgState, ARM_CMN_CFG)

#define ARM_CMN_CFG_DEFAULT_SIZE      0x40000000ULL
#define ARM_CMN_CFG_SEED_SIZE         0x03000000ULL
#define ARM_CMN_TRACE_SIZE            0x40000ULL

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
#define CMN_CFG_UNIT_INFO_0           0x0900
#define CMN_CFG_UNIT_INFO_1           0x0908

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
#define CMN_NODE_TYPE_CCRA            0x0103
#define CMN_NODE_TYPE_CCHA            0x0104
#define CMN_NODE_TYPE_CCLA            0x0105
#define CMN_NODE_TYPE_HN_S            0x0200
#define CMN_NODE_TYPE_HN_S_MPAM_S     0x0201
#define CMN_NODE_TYPE_HN_S_MPAM_NS    0x0202

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
#define CMN_CHILD_PTR_EXTERNAL_POS    31
#define CMN_MXP_PORT_CAL_CONNECTED    0x80U

typedef struct ArmCmnCfgState {
    SysBusDevice parent_obj;

    MemoryRegion cfg;
    MemoryRegion trace;
    char *shm_path;
    char *trace_shm_path;
    char *role;
    uint64_t cfg_size;
    uint8_t *cfg_ptr;
    uint8_t *trace_ptr;
} ArmCmnCfgState;

enum ArmCmnCfgRole {
    ARM_CMN_ROLE_AP,
    ARM_CMN_ROLE_SCP,
};

enum ArmCmnTraceOp {
    ARM_CMN_TRACE_READ = 0,
    ARM_CMN_TRACE_WRITE = 1,
};

struct ArmCmnTraceHeader {
    uint64_t magic;
    uint64_t producer;
    uint64_t consumer;
    uint64_t capacity;
};

struct ArmCmnTraceEntry {
    uint64_t seq;
    uint64_t info;
    uint64_t addr;
    uint64_t value;
};

#define ARM_CMN_TRACE_MAGIC          0x434d4e5452414345ULL
#define ARM_CMN_TRACE_ENTRY_COUNT    4096U
#define ARM_CMN_TRACE_ENTRY_BASE     0x100U
#define ARM_CMN_TRACE_ENTRY_STRIDE   0x20U
#define ARM_CMN_TRACE_REG_MAGIC      0x000
#define ARM_CMN_TRACE_REG_PRODUCER   0x008
#define ARM_CMN_TRACE_REG_CONSUMER   0x010
#define ARM_CMN_TRACE_REG_CAPACITY   0x018

struct CmnPortSeed {
    uint8_t device_type;
    bool cal_connected;
};

struct CmnChildSeed {
    uint32_t offset;
    uint16_t node_type;
    uint16_t node_id;
    uint16_t ldid;
    bool external;
};

struct CmnXpSeed {
    uint32_t offset;
    uint16_t node_id;
    uint16_t ldid;
    uint8_t port_count;
    struct CmnPortSeed ports[6];
    const struct CmnChildSeed *children;
    size_t child_count;
};

enum CmnInternalKind {
    CMN_INTERNAL_CFG,
    CMN_INTERNAL_DT,
    CMN_INTERNAL_DN,
    CMN_INTERNAL_APB,
    CMN_INTERNAL_RNI_RNSAM,
};

struct CmnInternalSeed {
    uint32_t offset;
    enum CmnInternalKind kind;
    uint16_t node_id;
    uint16_t ldid;
    uint16_t owner_type;
    const uint32_t *children;
    uint16_t child_count;
};

static const struct CmnChildSeed cmn_xp00_children[] = {
    { .offset = 0x0100000, .node_type = CMN_NODE_TYPE_SBSX, .node_id = 0, .ldid = 0, .external = false },
    { .offset = 0x0180000, .node_type = CMN_NODE_TYPE_HN_I, .node_id = 2, .ldid = 0, .external = false },
    { .offset = 0x0210000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 4, .ldid = 0, .external = false },
    { .offset = 0x0110000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 4, .ldid = 0, .external = false },
    { .offset = 0x0390000, .node_type = CMN_NODE_TYPE_CCRA, .node_id = 6, .ldid = 0, .external = false },
    { .offset = 0x0290000, .node_type = CMN_NODE_TYPE_CCLA, .node_id = 6, .ldid = 0, .external = false },
    { .offset = 0x0090000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 6, .ldid = 1, .external = false },
    { .offset = 0x0190000, .node_type = CMN_NODE_TYPE_CCHA, .node_id = 6, .ldid = 0, .external = false },
    { .offset = 0x01d0000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 7, .ldid = 1, .external = false },
};

static const struct CmnChildSeed cmn_xp01_children[] = {
    { .offset = 0x04a0000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 8, .ldid = 2, .external = true },
    { .offset = 0x0780000, .node_type = CMN_NODE_TYPE_HN_S, .node_id = 10, .ldid = 0, .external = false },
    { .offset = 0x0580000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_S, .node_id = 10, .ldid = 0, .external = false },
    { .offset = 0x0680000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_NS, .node_id = 10, .ldid = 0, .external = false },
    { .offset = 0x07c0000, .node_type = CMN_NODE_TYPE_HN_S, .node_id = 11, .ldid = 1, .external = false },
    { .offset = 0x05c0000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_S, .node_id = 11, .ldid = 1, .external = false },
    { .offset = 0x06c0000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_NS, .node_id = 11, .ldid = 1, .external = false },
    { .offset = 0x0710000, .node_type = CMN_NODE_TYPE_CCRA, .node_id = 12, .ldid = 1, .external = false },
    { .offset = 0x0610000, .node_type = CMN_NODE_TYPE_CCLA, .node_id = 12, .ldid = 1, .external = false },
    { .offset = 0x0410000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 12, .ldid = 3, .external = false },
    { .offset = 0x0510000, .node_type = CMN_NODE_TYPE_CCHA, .node_id = 12, .ldid = 1, .external = false },
    { .offset = 0x0550000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 13, .ldid = 2, .external = false },
};

static const struct CmnChildSeed cmn_xp02_children[] = {
    { .offset = 0x0980000, .node_type = CMN_NODE_TYPE_SBSX, .node_id = 18, .ldid = 1, .external = false },
    { .offset = 0x09c0000, .node_type = CMN_NODE_TYPE_SBSX, .node_id = 19, .ldid = 2, .external = false },
    { .offset = 0x0b10000, .node_type = CMN_NODE_TYPE_HN_I, .node_id = 20, .ldid = 1, .external = false },
    { .offset = 0x0b50000, .node_type = CMN_NODE_TYPE_HN_I, .node_id = 21, .ldid = 2, .external = false },
    { .offset = 0x0a90000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 22, .ldid = 4, .external = false },
    { .offset = 0x0990000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 22, .ldid = 3, .external = false },
};

static const struct CmnChildSeed cmn_xp10_children[] = {
    { .offset = 0x1200000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 32, .ldid = 5, .external = false },
    { .offset = 0x1100000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 32, .ldid = 4, .external = false },
    { .offset = 0x1380000, .node_type = CMN_NODE_TYPE_HN_I, .node_id = 34, .ldid = 3, .external = false },
    { .offset = 0x1310000, .node_type = CMN_NODE_TYPE_CCRA, .node_id = 36, .ldid = 2, .external = false },
    { .offset = 0x1210000, .node_type = CMN_NODE_TYPE_CCLA, .node_id = 36, .ldid = 2, .external = false },
    { .offset = 0x1010000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 36, .ldid = 6, .external = false },
    { .offset = 0x1110000, .node_type = CMN_NODE_TYPE_CCHA, .node_id = 36, .ldid = 2, .external = false },
    { .offset = 0x1150000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 37, .ldid = 5, .external = false },
};

static const struct CmnChildSeed cmn_xp11_children[] = {
    { .offset = 0x14a0000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 40, .ldid = 7, .external = true },
    { .offset = 0x1710000, .node_type = CMN_NODE_TYPE_HN_S, .node_id = 44, .ldid = 2, .external = false },
    { .offset = 0x1510000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_S, .node_id = 44, .ldid = 2, .external = false },
    { .offset = 0x1610000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_NS, .node_id = 44, .ldid = 2, .external = false },
    { .offset = 0x1750000, .node_type = CMN_NODE_TYPE_HN_S, .node_id = 45, .ldid = 3, .external = false },
    { .offset = 0x1550000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_S, .node_id = 45, .ldid = 3, .external = false },
    { .offset = 0x1650000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_NS, .node_id = 45, .ldid = 3, .external = false },
};

static const struct CmnChildSeed cmn_xp12_children[] = {
    { .offset = 0x1980000, .node_type = CMN_NODE_TYPE_SBSX, .node_id = 50, .ldid = 3, .external = false },
    { .offset = 0x19c0000, .node_type = CMN_NODE_TYPE_SBSX, .node_id = 51, .ldid = 4, .external = false },
    { .offset = 0x1a10000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 52, .ldid = 8, .external = false },
    { .offset = 0x1910000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 52, .ldid = 6, .external = false },
};

static const struct CmnChildSeed cmn_xp20_children[] = {
    { .offset = 0x20a0000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 64, .ldid = 9, .external = true },
    { .offset = 0x2300000, .node_type = CMN_NODE_TYPE_HN_S, .node_id = 66, .ldid = 4, .external = false },
    { .offset = 0x2100000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_S, .node_id = 66, .ldid = 4, .external = false },
    { .offset = 0x2200000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_NS, .node_id = 66, .ldid = 4, .external = false },
    { .offset = 0x23c0000, .node_type = CMN_NODE_TYPE_HN_S, .node_id = 67, .ldid = 5, .external = false },
    { .offset = 0x21c0000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_S, .node_id = 67, .ldid = 5, .external = false },
    { .offset = 0x22c0000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_NS, .node_id = 67, .ldid = 5, .external = false },
    { .offset = 0x2310000, .node_type = CMN_NODE_TYPE_HN_I, .node_id = 68, .ldid = 4, .external = false },
    { .offset = 0x2390000, .node_type = CMN_NODE_TYPE_CCRA, .node_id = 70, .ldid = 3, .external = false },
    { .offset = 0x2290000, .node_type = CMN_NODE_TYPE_CCLA, .node_id = 70, .ldid = 3, .external = false },
    { .offset = 0x2090000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 70, .ldid = 10, .external = false },
    { .offset = 0x2190000, .node_type = CMN_NODE_TYPE_CCHA, .node_id = 70, .ldid = 3, .external = false },
    { .offset = 0x21d0000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 71, .ldid = 7, .external = false },
};

static const struct CmnChildSeed cmn_xp21_children[] = {
    { .offset = 0x24a0000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 72, .ldid = 11, .external = true },
    { .offset = 0x2780000, .node_type = CMN_NODE_TYPE_HN_S, .node_id = 74, .ldid = 6, .external = false },
    { .offset = 0x2580000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_S, .node_id = 74, .ldid = 6, .external = false },
    { .offset = 0x2680000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_NS, .node_id = 74, .ldid = 6, .external = false },
    { .offset = 0x27c0000, .node_type = CMN_NODE_TYPE_HN_S, .node_id = 75, .ldid = 7, .external = false },
    { .offset = 0x25c0000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_S, .node_id = 75, .ldid = 7, .external = false },
    { .offset = 0x26c0000, .node_type = CMN_NODE_TYPE_HN_S_MPAM_NS, .node_id = 75, .ldid = 7, .external = false },
    { .offset = 0x2610000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 76, .ldid = 12, .external = false },
    { .offset = 0x2510000, .node_type = CMN_NODE_TYPE_RN_I, .node_id = 76, .ldid = 8, .external = false },
};

static const struct CmnChildSeed cmn_xp22_children[] = {
    { .offset = 0x2980000, .node_type = CMN_NODE_TYPE_SBSX, .node_id = 82, .ldid = 5, .external = false },
    { .offset = 0x29c0000, .node_type = CMN_NODE_TYPE_SBSX, .node_id = 83, .ldid = 6, .external = false },
    { .offset = 0x2a10000, .node_type = CMN_NODE_TYPE_RN_SAM, .node_id = 84, .ldid = 13, .external = false },
    { .offset = 0x2910000, .node_type = CMN_NODE_TYPE_RN_D, .node_id = 84, .ldid = 0, .external = false },
    { .offset = 0x2b90000, .node_type = CMN_NODE_TYPE_HN_I, .node_id = 86, .ldid = 5, .external = false },
};

static const struct CmnXpSeed cmn_mesh[] = {
    {
        .offset = 0x0020000,
        .node_id = 7,
        .ldid = 0,
        .port_count = 4,
        .ports = {
            { .device_type = CMN_DEVTYPE_SBSX, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_HN_D, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_RN_I, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_CCG, .cal_connected = false },
        },
        .children = cmn_xp00_children,
        .child_count = ARRAY_SIZE(cmn_xp00_children),
    },
    {
        .offset = 0x0420000,
        .node_id = 15,
        .ldid = 1,
        .port_count = 3,
        .ports = {
            { .device_type = CMN_DEVTYPE_RNF_ESAM, .cal_connected = true },
            { .device_type = CMN_DEVTYPE_HN_S, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_CCG, .cal_connected = false },
        },
        .children = cmn_xp01_children,
        .child_count = ARRAY_SIZE(cmn_xp01_children),
    },
    {
        .offset = 0x0820000,
        .node_id = 23,
        .ldid = 2,
        .port_count = 4,
        .ports = {
            { .device_type = CMN_DEVTYPE_SNF, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_SBSX, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_HN_I, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_RN_I, .cal_connected = false },
        },
        .children = cmn_xp02_children,
        .child_count = ARRAY_SIZE(cmn_xp02_children),
    },
    {
        .offset = 0x1020000,
        .node_id = 39,
        .ldid = 3,
        .port_count = 3,
        .ports = {
            { .device_type = CMN_DEVTYPE_RN_I, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_HN_P, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_CCG, .cal_connected = false },
        },
        .children = cmn_xp10_children,
        .child_count = ARRAY_SIZE(cmn_xp10_children),
    },
    {
        .offset = 0x1420000,
        .node_id = 47,
        .ldid = 4,
        .port_count = 2,
        .ports = {
            { .device_type = CMN_DEVTYPE_RNF_ESAM, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_HN_S, .cal_connected = false },
        },
        .children = cmn_xp11_children,
        .child_count = ARRAY_SIZE(cmn_xp11_children),
    },
    {
        .offset = 0x1820000,
        .node_id = 55,
        .ldid = 5,
        .port_count = 3,
        .ports = {
            { .device_type = CMN_DEVTYPE_SNF, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_SBSX, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_RN_I, .cal_connected = false },
        },
        .children = cmn_xp12_children,
        .child_count = ARRAY_SIZE(cmn_xp12_children),
    },
    {
        .offset = 0x2020000,
        .node_id = 71,
        .ldid = 6,
        .port_count = 4,
        .ports = {
            { .device_type = CMN_DEVTYPE_RNF_ESAM, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_HN_S, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_HN_P, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_CCG, .cal_connected = false },
        },
        .children = cmn_xp20_children,
        .child_count = ARRAY_SIZE(cmn_xp20_children),
    },
    {
        .offset = 0x2420000,
        .node_id = 79,
        .ldid = 7,
        .port_count = 3,
        .ports = {
            { .device_type = CMN_DEVTYPE_RNF_ESAM, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_HN_S, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_RN_I, .cal_connected = false },
        },
        .children = cmn_xp21_children,
        .child_count = ARRAY_SIZE(cmn_xp21_children),
    },
    {
        .offset = 0x2820000,
        .node_id = 87,
        .ldid = 8,
        .port_count = 4,
        .ports = {
            { .device_type = CMN_DEVTYPE_SNF, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_SBSX, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_RN_D, .cal_connected = false },
            { .device_type = CMN_DEVTYPE_HN_P, .cal_connected = false },
        },
        .children = cmn_xp22_children,
        .child_count = ARRAY_SIZE(cmn_xp22_children),
    },
};

static const struct CmnInternalSeed cmn_internal_windows[] = {
    {
        .offset = 0x0380000,
        .kind = CMN_INTERNAL_DT,
        .node_id = 2,
        .ldid = 0,
        .owner_type = CMN_NODE_TYPE_HN_I,
    },
    {
        .offset = 0x0280000,
        .kind = CMN_INTERNAL_DN,
        .node_id = 2,
        .ldid = 0,
        .owner_type = CMN_NODE_TYPE_HN_I,
    },
    {
        .offset = 0x0002000,
        .kind = CMN_INTERNAL_APB,
        .node_id = 2,
        .ldid = 0,
        .owner_type = CMN_NODE_TYPE_HN_I,
    },
    {
        .offset = 0x02d0000,
        .kind = CMN_INTERNAL_RNI_RNSAM,
        .node_id = 7,
        .ldid = 1,
        .owner_type = CMN_NODE_TYPE_RN_I,
    },
    {
        .offset = 0x0650000,
        .kind = CMN_INTERNAL_RNI_RNSAM,
        .node_id = 13,
        .ldid = 2,
        .owner_type = CMN_NODE_TYPE_RN_I,
    },
    {
        .offset = 0x1250000,
        .kind = CMN_INTERNAL_RNI_RNSAM,
        .node_id = 37,
        .ldid = 5,
        .owner_type = CMN_NODE_TYPE_RN_I,
    },
    {
        .offset = 0x22d0000,
        .kind = CMN_INTERNAL_RNI_RNSAM,
        .node_id = 71,
        .ldid = 7,
        .owner_type = CMN_NODE_TYPE_RN_I,
    },
};

static enum ArmCmnCfgRole arm_cmn_cfg_role(const ArmCmnCfgState *s)
{
    return (s->role != NULL && strcmp(s->role, "scp") == 0) ?
        ARM_CMN_ROLE_SCP :
        ARM_CMN_ROLE_AP;
}

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

static uint64_t cmn_make_child_ptr(const struct CmnChildSeed *child)
{
    uint64_t value = child->offset;

    if (child->external) {
        value |= 1ULL << CMN_CHILD_PTR_EXTERNAL_POS;
    }

    return value;
}

static uint64_t cmn_make_port_info(const struct CmnPortSeed *port)
{
    uint64_t value = port->device_type;

    if (port->cal_connected) {
        value |= CMN_MXP_PORT_CAL_CONNECTED;
    }

    return value;
}

static uint16_t cmn_internal_kind_node_type(enum CmnInternalKind kind)
{
    switch (kind) {
    case CMN_INTERNAL_CFG:
        return CMN_NODE_TYPE_CFG_ROOT;
    case CMN_INTERNAL_DT:
        return 0x0003;
    case CMN_INTERNAL_DN:
        return 0x0001;
    case CMN_INTERNAL_APB:
        return CMN_NODE_TYPE_CFG_ROOT;
    case CMN_INTERNAL_RNI_RNSAM:
        return CMN_NODE_TYPE_RN_SAM;
    default:
        g_assert_not_reached();
    }
}

static uint64_t cmn_make_internal_tag0(const struct CmnInternalSeed *seed)
{
    return 0x434d4e0000000000ULL |
        ((uint64_t)seed->kind << 32) |
        ((uint64_t)seed->owner_type << 16) |
        seed->node_id;
}

static uint64_t cmn_make_internal_tag1(const struct CmnInternalSeed *seed)
{
    return ((uint64_t)seed->ldid << 48) |
        ((uint64_t)seed->child_count << 32) |
        seed->offset;
}

static void cmn_writeq(uint8_t *cfg, uint32_t offset, uint64_t value)
{
    stq_le_p(cfg + offset, value);
}

static uint64_t arm_cmn_cfg_load(const uint8_t *base, hwaddr addr,
                                 unsigned size)
{
    switch (size) {
    case 1:
        return ldub_p(base + addr);
    case 2:
        return lduw_le_p(base + addr);
    case 4:
        return ldl_le_p(base + addr);
    case 8:
        return ldq_le_p(base + addr);
    default:
        g_assert_not_reached();
    }
}

static void arm_cmn_cfg_store(uint8_t *base, hwaddr addr, uint64_t value,
                              unsigned size)
{
    switch (size) {
    case 1:
        stb_p(base + addr, value);
        break;
    case 2:
        stw_le_p(base + addr, value);
        break;
    case 4:
        stl_le_p(base + addr, value);
        break;
    case 8:
        stq_le_p(base + addr, value);
        break;
    default:
        g_assert_not_reached();
    }
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
                   cmn_mesh[i].offset);
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
    size_t i, j;

    for (i = 0; i < ARRAY_SIZE(cmn_mesh); ++i) {
        const struct CmnXpSeed *xp = &cmn_mesh[i];
        uint32_t xp_offset = xp->offset;

        g_assert((uint64_t)xp_offset + 0x10000 <= seed_size);

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
                       cmn_make_port_info(&xp->ports[j]));
        }

        for (j = 0; j < xp->child_count; ++j) {
            const struct CmnChildSeed *node = &xp->children[j];
            uint32_t node_offset = node->offset;

            g_assert((uint64_t)node_offset + 0x10000 <= seed_size);

            cmn_writeq(cfg,
                       xp_offset + CMN_MXP_CHILD_PTR_BASE +
                           (j * sizeof(uint64_t)),
                       cmn_make_child_ptr(node));

            cmn_writeq(cfg,
                       node_offset + CMN_CFG_NODE_INFO,
                       cmn_make_node_info(node->node_type,
                                          node->node_id,
                                          node->ldid));
            cmn_writeq(cfg, node_offset + CMN_NODE_CHILD_INFO, 0);
        }
    }
}

static void cmn_seed_internal_windows(uint8_t *cfg, uint64_t seed_size)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(cmn_internal_windows); ++i) {
        const struct CmnInternalSeed *seed = &cmn_internal_windows[i];
        uint32_t offset = seed->offset;
        size_t j;

        g_assert((uint64_t)offset + 0x1000 <= seed_size);

        cmn_writeq(cfg,
                   offset + CMN_CFG_NODE_INFO,
                   cmn_make_node_info(cmn_internal_kind_node_type(seed->kind),
                                      seed->node_id,
                                      seed->ldid));
        cmn_writeq(cfg,
                   offset + CMN_CFG_UNIT_INFO_0,
                   cmn_make_internal_tag0(seed));
        cmn_writeq(cfg,
                   offset + CMN_CFG_UNIT_INFO_1,
                   cmn_make_internal_tag1(seed));

        if (seed->child_count == 0) {
            cmn_writeq(cfg, offset + CMN_CFG_CHILD_INFO, 0);
            continue;
        }

        cmn_writeq(cfg,
                   offset + CMN_CFG_CHILD_INFO,
                   cmn_make_child_info(seed->child_count));
        for (j = 0; j < seed->child_count; ++j) {
            cmn_writeq(cfg,
                       offset + CMN_CFG_CHILD_PTR_BASE +
                           (j * sizeof(uint64_t)),
                       seed->children[j]);
        }
    }
}

static void arm_cmn_cfg_seed_window(uint8_t *cfg, uint64_t seed_size)
{
    memset(cfg, 0, seed_size);
    cmn_seed_root_registers(cfg);
    cmn_seed_mesh(cfg, seed_size);
    cmn_seed_internal_windows(cfg, seed_size);
}

static void arm_cmn_trace_reset(uint8_t *trace)
{
    struct ArmCmnTraceHeader *hdr = (struct ArmCmnTraceHeader *)trace;

    memset(trace, 0, ARM_CMN_TRACE_SIZE);
    hdr->magic = ARM_CMN_TRACE_MAGIC;
    hdr->capacity = ARM_CMN_TRACE_ENTRY_COUNT;
}

static void arm_cmn_trace_log(ArmCmnCfgState *s, uint64_t addr, uint64_t value,
                              unsigned size, enum ArmCmnTraceOp op)
{
    struct ArmCmnTraceHeader *hdr;
    struct ArmCmnTraceEntry *entry;
    uint64_t seq;
    uint64_t idx;

    if (arm_cmn_cfg_role(s) != ARM_CMN_ROLE_SCP || s->trace_ptr == NULL) {
        return;
    }

    hdr = (struct ArmCmnTraceHeader *)s->trace_ptr;
    if (hdr->magic != ARM_CMN_TRACE_MAGIC || hdr->capacity == 0) {
        return;
    }

    seq = hdr->producer;
    idx = seq % hdr->capacity;
    entry = (struct ArmCmnTraceEntry *)(s->trace_ptr + ARM_CMN_TRACE_ENTRY_BASE +
        (idx * ARM_CMN_TRACE_ENTRY_STRIDE));

    entry->seq = seq;
    entry->info = ((uint64_t)size << 8) | op;
    entry->addr = addr;
    entry->value = value;
    hdr->producer = seq + 1;
}

static uint64_t arm_cmn_cfg_read(void *opaque, hwaddr addr, unsigned size)
{
    ArmCmnCfgState *s = opaque;
    uint64_t value;

    value = arm_cmn_cfg_load(s->cfg_ptr, addr, size);
    arm_cmn_trace_log(s, addr, value, size, ARM_CMN_TRACE_READ);
    return value;
}

static void arm_cmn_cfg_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    ArmCmnCfgState *s = opaque;

    arm_cmn_cfg_store(s->cfg_ptr, addr, value, size);
    arm_cmn_trace_log(s, addr, value, size, ARM_CMN_TRACE_WRITE);
}

static uint64_t arm_cmn_trace_read(void *opaque, hwaddr addr, unsigned size)
{
    ArmCmnCfgState *s = opaque;

    return arm_cmn_cfg_load(s->trace_ptr, addr, size);
}

static void arm_cmn_trace_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    ArmCmnCfgState *s = opaque;

    if (addr == ARM_CMN_TRACE_REG_CONSUMER && size == 8) {
        stq_le_p(s->trace_ptr + ARM_CMN_TRACE_REG_CONSUMER, value);
        return;
    }

    arm_cmn_cfg_store(s->trace_ptr, addr, value, size);
}

static const MemoryRegionOps arm_cmn_cfg_ops = {
    .read = arm_cmn_cfg_read,
    .write = arm_cmn_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static const MemoryRegionOps arm_cmn_trace_ops = {
    .read = arm_cmn_trace_read,
    .write = arm_cmn_trace_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static bool arm_cmn_cfg_map_file(const char *path, uint64_t size, bool truncate,
                                 uint8_t **ptr_out, Error **errp)
{
    int fd;
    int open_flags = O_CREAT | O_RDWR;
    void *ptr;

    if (truncate) {
        open_flags |= O_TRUNC;
    }

    fd = open(path, open_flags, 0666);
    if (fd < 0) {
        error_setg(errp, "%s: open(%s) failed: %s",
                   TYPE_ARM_CMN_CFG, path, strerror(errno));
        return false;
    }

    if (ftruncate(fd, size) < 0) {
        error_setg(errp, "%s: ftruncate(%s) failed: %s",
                   TYPE_ARM_CMN_CFG, path, strerror(errno));
        close(fd);
        return false;
    }

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        error_setg(errp, "%s: mmap(%s) failed: %s",
                   TYPE_ARM_CMN_CFG, path, strerror(errno));
        close(fd);
        return false;
    }

    close(fd);
    *ptr_out = ptr;
    return true;
}

static bool arm_cmn_cfg_init_window(ArmCmnCfgState *s, Error **errp)
{
    bool seed_cfg = (arm_cmn_cfg_role(s) == ARM_CMN_ROLE_SCP);

    if (!arm_cmn_cfg_map_file(s->shm_path, s->cfg_size, false, &s->cfg_ptr,
                              errp)) {
        return false;
    }

    if (seed_cfg || ldq_le_p(s->cfg_ptr + CMN_CFG_NODE_INFO) !=
            cmn_make_node_info(CMN_NODE_TYPE_CFG_ROOT, 0, 0)) {
        arm_cmn_cfg_seed_window(s->cfg_ptr, ARM_CMN_CFG_SEED_SIZE);
    }

    if (!arm_cmn_cfg_map_file(s->trace_shm_path, ARM_CMN_TRACE_SIZE,
                              seed_cfg, &s->trace_ptr, errp)) {
        munmap(s->cfg_ptr, s->cfg_size);
        s->cfg_ptr = NULL;
        return false;
    }

    if (seed_cfg ||
        ldq_le_p(s->trace_ptr + ARM_CMN_TRACE_REG_MAGIC) != ARM_CMN_TRACE_MAGIC) {
        arm_cmn_trace_reset(s->trace_ptr);
    }

    return true;
}

static void arm_cmn_cfg_realize(DeviceState *dev, Error **errp)
{
    ArmCmnCfgState *s = ARM_CMN_CFG(dev);

    if (s->shm_path == NULL || s->trace_shm_path == NULL || s->cfg_size == 0) {
        error_setg(errp, "%s: shm-path, trace-shm-path and cfg-size must be set",
                   TYPE_ARM_CMN_CFG);
        return;
    }

    if (!arm_cmn_cfg_init_window(s, errp)) {
        return;
    }

    memory_region_init_io(&s->cfg, OBJECT(s), &arm_cmn_cfg_ops, s,
                          TYPE_ARM_CMN_CFG ".cfg", s->cfg_size);
    memory_region_init_io(&s->trace, OBJECT(s), &arm_cmn_trace_ops, s,
                          TYPE_ARM_CMN_CFG ".trace", ARM_CMN_TRACE_SIZE);
}

static void arm_cmn_cfg_unrealize(DeviceState *dev)
{
    ArmCmnCfgState *s = ARM_CMN_CFG(dev);

    if (s->cfg_ptr != NULL) {
        munmap(s->cfg_ptr, s->cfg_size);
        s->cfg_ptr = NULL;
    }

    if (s->trace_ptr != NULL) {
        munmap(s->trace_ptr, ARM_CMN_TRACE_SIZE);
        s->trace_ptr = NULL;
    }
}

static void arm_cmn_cfg_init(Object *obj)
{
    ArmCmnCfgState *s = ARM_CMN_CFG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    sysbus_init_mmio(sbd, &s->cfg);
    sysbus_init_mmio(sbd, &s->trace);
}

static const VMStateDescription vmstate_arm_cmn_cfg = {
    .name = TYPE_ARM_CMN_CFG,
    .unmigratable = 1,
};

static const Property arm_cmn_cfg_properties[] = {
    DEFINE_PROP_STRING("shm-path", ArmCmnCfgState, shm_path),
    DEFINE_PROP_STRING("trace-shm-path", ArmCmnCfgState, trace_shm_path),
    DEFINE_PROP_STRING("role", ArmCmnCfgState, role),
    DEFINE_PROP_UINT64("cfg-size", ArmCmnCfgState, cfg_size,
                       ARM_CMN_CFG_DEFAULT_SIZE),
};

static void arm_cmn_cfg_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, arm_cmn_cfg_properties);
    dc->realize = arm_cmn_cfg_realize;
    dc->unrealize = arm_cmn_cfg_unrealize;
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
