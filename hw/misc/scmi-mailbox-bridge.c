/*
 * SCMI mailbox bridge device for cross-QEMU experiments.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "system/memory.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TYPE_SCMI_MAILBOX_BRIDGE "scmi-mailbox-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(ScmiMailboxBridgeState, SCMI_MAILBOX_BRIDGE)

enum {
    REG_VERSION = 0x00,
    REG_STATUS = 0x04,
    REG_DOORBELL = 0x08,
    REG_IRQ_ACK = 0x0c,
    REG_SHM_SIZE = 0x10,
};

#define STATUS_IRQ_PENDING BIT(0)
#define VERSION_1 0x00010000u

typedef struct ScmiMailboxBridgeState {
    SysBusDevice parent_obj;

    MemoryRegion regs;
    MemoryRegion shm;
    qemu_irq irq;

    char *local_socket_path;
    char *peer_socket_path;
    char *shm_path;
    uint64_t shm_size;

    int sock_fd;
    uint32_t status;
} ScmiMailboxBridgeState;

static uint64_t scmi_mailbox_bridge_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    ScmiMailboxBridgeState *s = opaque;

    switch (addr) {
    case REG_VERSION:
        return VERSION_1;
    case REG_STATUS:
        return s->status;
    case REG_SHM_SIZE:
        return s->shm_size;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unsupported read addr=0x%" HWADDR_PRIx "\n",
                      TYPE_SCMI_MAILBOX_BRIDGE, addr);
        return 0;
    }
}

static void scmi_mailbox_bridge_kick_peer(ScmiMailboxBridgeState *s)
{
    struct sockaddr_un peer = { 0 };
    char notify = 1;
    ssize_t ret;

    if (s->peer_socket_path == NULL || s->sock_fd < 0) {
        return;
    }

    peer.sun_family = AF_UNIX;
    pstrcpy(peer.sun_path, sizeof(peer.sun_path), s->peer_socket_path);

    ret = sendto(s->sock_fd, &notify, sizeof(notify), 0,
                 (const struct sockaddr *)&peer, sizeof(peer));
    if (ret < 0 && errno != ENOENT && errno != ECONNREFUSED) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: sendto(%s) failed: %s\n",
                      TYPE_SCMI_MAILBOX_BRIDGE, s->peer_socket_path,
                      strerror(errno));
    }
}

static void scmi_mailbox_bridge_write(void *opaque, hwaddr addr, uint64_t value,
                                      unsigned size)
{
    ScmiMailboxBridgeState *s = opaque;

    switch (addr) {
    case REG_DOORBELL:
        if (value & 1) {
            scmi_mailbox_bridge_kick_peer(s);
        }
        break;
    case REG_IRQ_ACK:
        if (value & 1) {
            s->status &= ~STATUS_IRQ_PENDING;
            qemu_set_irq(s->irq, 0);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unsupported write addr=0x%" HWADDR_PRIx
                      " value=0x%" PRIx64 "\n",
                      TYPE_SCMI_MAILBOX_BRIDGE, addr, value);
        break;
    }
}

static const MemoryRegionOps scmi_mailbox_bridge_ops = {
    .read = scmi_mailbox_bridge_read,
    .write = scmi_mailbox_bridge_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void scmi_mailbox_bridge_socket_read(void *opaque)
{
    ScmiMailboxBridgeState *s = opaque;
    char buf[16];

    for (;;) {
        ssize_t ret = recv(s->sock_fd, buf, sizeof(buf), 0);

        if (ret > 0) {
            s->status |= STATUS_IRQ_PENDING;
            qemu_set_irq(s->irq, 1);
            continue;
        }

        if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        if (ret < 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: recv failed: %s\n",
                          TYPE_SCMI_MAILBOX_BRIDGE, strerror(errno));
        }
        return;
    }
}

static bool scmi_mailbox_bridge_init_shm(ScmiMailboxBridgeState *s,
                                         Error **errp)
{
    int fd = -1;
    bool ok;

    fd = open(s->shm_path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        error_setg(errp, "%s: open(%s) failed: %s",
                   TYPE_SCMI_MAILBOX_BRIDGE, s->shm_path, strerror(errno));
        return false;
    }

    if (ftruncate(fd, s->shm_size) < 0) {
        error_setg(errp, "%s: ftruncate(%s) failed: %s",
                   TYPE_SCMI_MAILBOX_BRIDGE, s->shm_path, strerror(errno));
        goto fail;
    }

    ok = memory_region_init_ram_from_fd(&s->shm, OBJECT(s),
                                        TYPE_SCMI_MAILBOX_BRIDGE ".shm",
                                        s->shm_size, RAM_SHARED, fd, 0,
                                        errp);
    close(fd);
    return ok;

fail:
    if (fd >= 0) {
        close(fd);
    }
    return false;
}

static bool scmi_mailbox_bridge_init_socket(ScmiMailboxBridgeState *s,
                                            Error **errp)
{
    struct sockaddr_un local = { 0 };

    s->sock_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s->sock_fd < 0) {
        error_setg(errp, "%s: socket() failed: %s",
                   TYPE_SCMI_MAILBOX_BRIDGE, strerror(errno));
        return false;
    }

    if (!qemu_set_blocking(s->sock_fd, false, errp)) {
        close(s->sock_fd);
        s->sock_fd = -1;
        return false;
    }

    unlink(s->local_socket_path);

    local.sun_family = AF_UNIX;
    pstrcpy(local.sun_path, sizeof(local.sun_path), s->local_socket_path);

    if (bind(s->sock_fd, (const struct sockaddr *)&local, sizeof(local)) < 0) {
        error_setg(errp, "%s: bind(%s) failed: %s",
                   TYPE_SCMI_MAILBOX_BRIDGE, s->local_socket_path,
                   strerror(errno));
        close(s->sock_fd);
        s->sock_fd = -1;
        return false;
    }

    qemu_set_fd_handler(s->sock_fd, scmi_mailbox_bridge_socket_read, NULL, s);
    return true;
}

static void scmi_mailbox_bridge_realize(DeviceState *dev, Error **errp)
{
    ScmiMailboxBridgeState *s = SCMI_MAILBOX_BRIDGE(dev);

    if (s->local_socket_path == NULL || s->peer_socket_path == NULL ||
        s->shm_path == NULL || s->shm_size == 0) {
        error_setg(errp, "%s: local-socket-path, peer-socket-path, shm-path "
                   "and shm-size must be set",
                   TYPE_SCMI_MAILBOX_BRIDGE);
        return;
    }

    memory_region_init_io(&s->regs, OBJECT(s), &scmi_mailbox_bridge_ops, s,
                          TYPE_SCMI_MAILBOX_BRIDGE ".regs", 0x1000);

    if (!scmi_mailbox_bridge_init_shm(s, errp)) {
        return;
    }

    if (!scmi_mailbox_bridge_init_socket(s, errp)) {
        object_unparent(OBJECT(&s->shm));
        return;
    }
}

static void scmi_mailbox_bridge_unrealize(DeviceState *dev)
{
    ScmiMailboxBridgeState *s = SCMI_MAILBOX_BRIDGE(dev);

    if (s->sock_fd >= 0) {
        qemu_set_fd_handler(s->sock_fd, NULL, NULL, NULL);
        close(s->sock_fd);
        s->sock_fd = -1;
    }

    if (s->local_socket_path != NULL) {
        unlink(s->local_socket_path);
    }
}

static void scmi_mailbox_bridge_reset(DeviceState *dev)
{
    ScmiMailboxBridgeState *s = SCMI_MAILBOX_BRIDGE(dev);

    s->status = 0;
    qemu_set_irq(s->irq, 0);
}

static void scmi_mailbox_bridge_init(Object *obj)
{
    ScmiMailboxBridgeState *s = SCMI_MAILBOX_BRIDGE(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->sock_fd = -1;

    sysbus_init_mmio(sbd, &s->regs);
    sysbus_init_mmio(sbd, &s->shm);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_scmi_mailbox_bridge = {
    .name = TYPE_SCMI_MAILBOX_BRIDGE,
    .unmigratable = 1,
};

static const Property scmi_mailbox_bridge_properties[] = {
    DEFINE_PROP_STRING("local-socket-path", ScmiMailboxBridgeState,
                       local_socket_path),
    DEFINE_PROP_STRING("peer-socket-path", ScmiMailboxBridgeState,
                       peer_socket_path),
    DEFINE_PROP_STRING("shm-path", ScmiMailboxBridgeState, shm_path),
    DEFINE_PROP_UINT64("shm-size", ScmiMailboxBridgeState, shm_size, 0x1000),
};

static void scmi_mailbox_bridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, scmi_mailbox_bridge_properties);
    device_class_set_legacy_reset(dc, scmi_mailbox_bridge_reset);
    dc->realize = scmi_mailbox_bridge_realize;
    dc->unrealize = scmi_mailbox_bridge_unrealize;
    dc->vmsd = &vmstate_scmi_mailbox_bridge;
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo scmi_mailbox_bridge_info = {
    .name = TYPE_SCMI_MAILBOX_BRIDGE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ScmiMailboxBridgeState),
    .instance_init = scmi_mailbox_bridge_init,
    .class_init = scmi_mailbox_bridge_class_init,
};

static void scmi_mailbox_bridge_register_types(void)
{
    type_register_static(&scmi_mailbox_bridge_info);
}

type_init(scmi_mailbox_bridge_register_types)
