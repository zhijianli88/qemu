/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */
#include "include/migration/migration.h"
#include "migration/migration-colo.h"
#include "net/net.h"
#include "net/colo-nic.h"
#include "qemu/error-report.h"
#include "net/tap.h"

typedef struct nic_device {
    COLONicState *cns;
    int (*configure)(COLONicState *cns, bool up, int side, int index);
    QTAILQ_ENTRY(nic_device) next;
    bool is_up;
} nic_device;

QTAILQ_HEAD(, nic_device) nic_devices = QTAILQ_HEAD_INITIALIZER(nic_devices);

static int colo_nic_configure(COLONicState *cns,
            bool up, int side, int index)
{
    int i, argc = 6;
    char *argv[7], index_str[32];
    char **parg;
    NetClientState *nc = container_of(cns, NetClientState, cns);
    TAPState *s = DO_UPCAST(TAPState, nc, nc);
    Error *err = NULL;

    if (!cns && index <= 0) {
        error_report("Can not parse colo_script or colo_nicname");
        return -1;
    }

    parg = argv;
    *parg++ = cns->script;
    *parg++ = (char *)(side == COLO_MODE_SECONDARY ? "secondary" : "primary");
    *parg++ = (char *)(up ? "install" : "uninstall");
    *parg++ = cns->nicname;
    *parg++ = cns->ifname;
    sprintf(index_str, "%d", index);
    *parg++ = index_str;
    *parg = NULL;

    for (i = 0; i < argc; i++) {
        if (!argv[i][0]) {
            error_report("Can not get colo_script argument");
            return -1;
        }
    }

    launch_script(argv, s->fd, &err);
    if (err) {
        error_report_err(err);
        return -1;
    }
    return 0;
}

static int configure_one_nic(COLONicState *cns,
             bool up, int side, int index)
{
    struct nic_device *nic;

    assert(cns);

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        if (nic->cns == cns) {
            if (up == nic->is_up) {
                return 0;
            }

            if (!nic->configure || (nic->configure(nic->cns, up, side, index) &&
                up)) {
                return -1;
            }
            nic->is_up = up;
            return 0;
        }
    }

    return -1;
}

static int configure_nic(int side, int index)
{
    struct nic_device *nic;

    if (QTAILQ_EMPTY(&nic_devices)) {
        return -1;
    }

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        if (configure_one_nic(nic->cns, 1, side, index)) {
            return -1;
        }
    }

    return 0;
}

static void teardown_nic(int side, int index)
{
    struct nic_device *nic;

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        configure_one_nic(nic->cns, 0, side, index);
    }
}

void colo_add_nic_devices(COLONicState *cns)
{
    struct nic_device *nic;
    NetClientState *nc = container_of(cns, NetClientState, cns);

    if (nc->info->type == NET_CLIENT_OPTIONS_KIND_HUBPORT ||
        nc->info->type == NET_CLIENT_OPTIONS_KIND_NIC) {
        return;
    }
    QTAILQ_FOREACH(nic, &nic_devices, next) {
        NetClientState *nic_nc = container_of(nic->cns, NetClientState, cns);
        if ((nic_nc->peer && nic_nc->peer == nc) ||
            (nc->peer && nc->peer == nic_nc)) {
            return;
        }
    }

    nic = g_malloc0(sizeof(*nic));
    nic->configure = colo_nic_configure;
    nic->cns = cns;

    QTAILQ_INSERT_TAIL(&nic_devices, nic, next);
}

void colo_remove_nic_devices(COLONicState *cns)
{
    struct nic_device *nic, *next_nic;

    QTAILQ_FOREACH_SAFE(nic, &nic_devices, next, next_nic) {
        if (nic->cns == cns) {
            configure_one_nic(cns, 0, get_colo_mode(), getpid());
            QTAILQ_REMOVE(&nic_devices, nic, next);
            g_free(nic);
        }
    }
}

int colo_proxy_init(enum COLOMode mode)
{
    int ret = -1;

    ret = configure_nic(mode, getpid());
    if (ret != 0) {
        error_report("excute colo-proxy-script failed");
    }

    return ret;
}

void colo_proxy_destroy(enum COLOMode mode)
{
    teardown_nic(mode, getpid());
}
