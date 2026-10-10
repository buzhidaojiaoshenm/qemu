/*
 * Shared Arm CMN configuration-space model.
 *
 * A repository tool compiles the topology YAML and TRM reset profile into a
 * compact CMN3 artifact.  This device validates that artifact before touching
 * shared state, then applies its register/value entries to the CMN aperture.
 * Functional CMN behavior is intentionally out of scope.
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
#include <zlib.h>

#define TYPE_ARM_CMN_CFG "arm-cmn-cfg"
OBJECT_DECLARE_SIMPLE_TYPE(ArmCmnCfgState, ARM_CMN_CFG)

#define ARM_CMN_CFG_DEFAULT_SIZE      0x40000000ULL
#define ARM_CMN_TRACE_SIZE            0x40000ULL

#define CMN3_HEADER_SIZE              64U
#define CMN3_ENTRY_SIZE               16U
#define CMN3_FORMAT_VERSION           1U

#define ARM_CMN_TRACE_MAGIC          0x434d4e5452414345ULL
#define ARM_CMN_TRACE_ENTRY_COUNT    4096U
#define ARM_CMN_TRACE_ENTRY_BASE     0x100U
#define ARM_CMN_TRACE_ENTRY_STRIDE   0x20U
#define ARM_CMN_TRACE_REG_MAGIC      0x000
#define ARM_CMN_TRACE_REG_PRODUCER   0x008
#define ARM_CMN_TRACE_REG_CONSUMER   0x010
#define ARM_CMN_TRACE_REG_CAPACITY   0x018

typedef struct ArmCmnCfgState {
    SysBusDevice parent_obj;

    MemoryRegion cfg;
    MemoryRegion trace;
    char *shm_path;
    char *trace_shm_path;
    char *topology_file;
    char *role;
    uint64_t cfg_size;
    uint8_t *cfg_ptr;
    uint8_t *trace_ptr;
} ArmCmnCfgState;

typedef struct Cmn3Entry {
    uint64_t offset;
    uint64_t value;
} Cmn3Entry;

typedef struct Cmn3Topology {
    uint32_t seed_extent;
    uint32_t entry_count;
    Cmn3Entry *entries;
} Cmn3Topology;

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

static enum ArmCmnCfgRole arm_cmn_cfg_role(const ArmCmnCfgState *s)
{
    return (s->role != NULL && strcmp(s->role, "scp") == 0) ?
        ARM_CMN_ROLE_SCP :
        ARM_CMN_ROLE_AP;
}

static void cmn3_topology_clear(Cmn3Topology *topology)
{
    g_free(topology->entries);
    topology->entries = NULL;
}

static uint32_t cmn3_payload_crc32(const uint8_t *payload, size_t length)
{
    uLong crc = crc32(0L, Z_NULL, 0);

    while (length != 0) {
        uInt chunk = length > UINT_MAX ? UINT_MAX : length;

        crc = crc32(crc, payload, chunk);
        payload += chunk;
        length -= chunk;
    }
    return crc;
}

static bool cmn3_load_topology(const ArmCmnCfgState *s,
                               Cmn3Topology *topology, Error **errp)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *data = NULL;
    const uint8_t *bytes;
    const uint8_t *payload;
    const uint8_t *version;
    const uint8_t *version_end;
    gsize length;
    uint32_t entry_count;
    uint32_t seed_extent;
    uint32_t expected_crc;
    uint64_t previous_offset = 0;

    if (s->topology_file == NULL || s->topology_file[0] == '\0') {
        error_setg(errp, "%s: topology-file must be set", TYPE_ARM_CMN_CFG);
        return false;
    }
    if (!g_file_get_contents(s->topology_file, &data, &length, &error)) {
        error_setg(errp, "%s: cannot read topology-file '%s': %s",
                   TYPE_ARM_CMN_CFG, s->topology_file, error->message);
        return false;
    }

    bytes = (const uint8_t *)data;
    if (length < CMN3_HEADER_SIZE) {
        error_setg(errp, "%s: topology artifact is shorter than its header",
                   TYPE_ARM_CMN_CFG);
        return false;
    }
    if (memcmp(bytes, "CMN3", 4) != 0) {
        error_setg(errp, "%s: topology artifact has invalid magic",
                   TYPE_ARM_CMN_CFG);
        return false;
    }
    if (lduw_le_p(bytes + 4) != CMN3_FORMAT_VERSION) {
        error_setg(errp, "%s: unsupported format version %u",
                   TYPE_ARM_CMN_CFG, lduw_le_p(bytes + 4));
        return false;
    }
    if (lduw_le_p(bytes + 6) != CMN3_HEADER_SIZE) {
        error_setg(errp, "%s: invalid CMN3 header size %u",
                   TYPE_ARM_CMN_CFG, lduw_le_p(bytes + 6));
        return false;
    }
    if (lduw_le_p(bytes + 8) == 0 || lduw_le_p(bytes + 10) == 0) {
        error_setg(errp, "%s: mesh dimensions must be nonzero",
                   TYPE_ARM_CMN_CFG);
        return false;
    }
    if (lduw_le_p(bytes + 14) != 0) {
        error_setg(errp, "%s: reserved field must be zero",
                   TYPE_ARM_CMN_CFG);
        return false;
    }

    seed_extent = ldl_le_p(bytes + 16);
    entry_count = ldl_le_p(bytes + 20);
    if (seed_extent == 0 || seed_extent > s->cfg_size) {
        error_setg(errp, "%s: seed extent exceeds cfg-size",
                   TYPE_ARM_CMN_CFG);
        return false;
    }
    if ((length - CMN3_HEADER_SIZE) % CMN3_ENTRY_SIZE != 0 ||
        (length - CMN3_HEADER_SIZE) / CMN3_ENTRY_SIZE != entry_count) {
        error_setg(errp,
                   "%s: topology artifact length does not match entry count",
                   TYPE_ARM_CMN_CFG);
        return false;
    }

    version = bytes + 32;
    version_end = memchr(version, '\0', 32);
    if (version_end == NULL ||
        !g_utf8_validate((const char *)version, version_end - version, NULL)) {
        error_setg(errp, "%s: version text is not valid NUL-terminated UTF-8",
                   TYPE_ARM_CMN_CFG);
        return false;
    }

    payload = bytes + CMN3_HEADER_SIZE;
    expected_crc = ldl_le_p(bytes + 28);
    if (cmn3_payload_crc32(payload, length - CMN3_HEADER_SIZE) !=
        expected_crc) {
        error_setg(errp, "%s: payload CRC32 mismatch", TYPE_ARM_CMN_CFG);
        return false;
    }

    topology->entries = g_new(Cmn3Entry, entry_count);
    topology->seed_extent = seed_extent;
    topology->entry_count = entry_count;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *encoded = payload + i * CMN3_ENTRY_SIZE;
        uint64_t offset = ldq_le_p(encoded);

        if (i != 0 && offset <= previous_offset) {
            error_setg(errp,
                       "%s: entry offsets must be strictly increasing",
                       TYPE_ARM_CMN_CFG);
            cmn3_topology_clear(topology);
            return false;
        }
        if (offset % sizeof(uint64_t) != 0) {
            error_setg(errp, "%s: entry offset must be 8-byte aligned",
                       TYPE_ARM_CMN_CFG);
            cmn3_topology_clear(topology);
            return false;
        }
        if (offset > seed_extent ||
            seed_extent - offset < sizeof(uint64_t)) {
            error_setg(errp, "%s: entry exceeds seed extent",
                       TYPE_ARM_CMN_CFG);
            cmn3_topology_clear(topology);
            return false;
        }

        topology->entries[i].offset = offset;
        topology->entries[i].value = ldq_le_p(encoded + 8);
        previous_offset = offset;
    }

    return true;
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

static void cmn3_seed_window(uint8_t *cfg, const Cmn3Topology *topology)
{
    memset(cfg, 0, topology->seed_extent);
    for (uint32_t i = 0; i < topology->entry_count; i++) {
        stq_le_p(cfg + topology->entries[i].offset,
                 topology->entries[i].value);
    }
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

    seq = qatomic_fetch_inc(&hdr->producer);
    idx = seq % hdr->capacity;
    entry = (struct ArmCmnTraceEntry *)(s->trace_ptr +
        ARM_CMN_TRACE_ENTRY_BASE + idx * ARM_CMN_TRACE_ENTRY_STRIDE);

    entry->info = ((uint64_t)op << 32) | size;
    entry->addr = addr;
    entry->value = value;
    /* Publish the entry payload before making its sequence visible. */
    smp_wmb();
    entry->seq = seq + 1;
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

static bool arm_cmn_cfg_init_window(ArmCmnCfgState *s,
                                    const Cmn3Topology *topology,
                                    Error **errp)
{
    bool scp_role = arm_cmn_cfg_role(s) == ARM_CMN_ROLE_SCP;

    if (!arm_cmn_cfg_map_file(s->shm_path, s->cfg_size, false, &s->cfg_ptr,
                              errp)) {
        return false;
    }

    if (scp_role || ldq_le_p(s->cfg_ptr) == 0) {
        cmn3_seed_window(s->cfg_ptr, topology);
    }

    if (!arm_cmn_cfg_map_file(s->trace_shm_path, ARM_CMN_TRACE_SIZE,
                              scp_role, &s->trace_ptr, errp)) {
        munmap(s->cfg_ptr, s->cfg_size);
        s->cfg_ptr = NULL;
        return false;
    }

    if (scp_role ||
        ldq_le_p(s->trace_ptr + ARM_CMN_TRACE_REG_MAGIC) !=
        ARM_CMN_TRACE_MAGIC) {
        arm_cmn_trace_reset(s->trace_ptr);
    }

    return true;
}

static void arm_cmn_cfg_realize(DeviceState *dev, Error **errp)
{
    ArmCmnCfgState *s = ARM_CMN_CFG(dev);
    Cmn3Topology topology = { 0 };

    if (s->shm_path == NULL || s->trace_shm_path == NULL || s->cfg_size == 0) {
        error_setg(errp,
                   "%s: shm-path, trace-shm-path and cfg-size must be set",
                   TYPE_ARM_CMN_CFG);
        return;
    }

    if (!cmn3_load_topology(s, &topology, errp)) {
        return;
    }
    if (!arm_cmn_cfg_init_window(s, &topology, errp)) {
        cmn3_topology_clear(&topology);
        return;
    }
    cmn3_topology_clear(&topology);

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
    DEFINE_PROP_STRING("topology-file", ArmCmnCfgState, topology_file),
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
