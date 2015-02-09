/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later. See the COPYING file in the top-level directory.
 *
 */

#include <migration/colo.h>
#include "trace.h"

typedef struct {
     bool skip;
     uint32_t colo_requested;
} COLOInfo;

static COLOInfo colo_info;

void savevm_skip_colo_state(void)
{
    colo_info.skip = true;
}

static void colo_info_pre_save(void *opaque)
{
    COLOInfo *s = opaque;

    if (migrate_enable_colo()) {
        s->colo_requested = 1;
    } else {
        s->colo_requested = 0;
    }
}

static bool colo_info_need(void *opaque)
{
    if (migrate_enable_colo() && !colo_info.skip) {
        return true;
     }
    return false;
}

static const VMStateDescription colo_state = {
     .name = "COLOState",
     .version_id = 1,
     .minimum_version_id = 1,
     .pre_save = colo_info_pre_save,
     .needed = colo_info_need,
     .fields = (VMStateField[]) {
         VMSTATE_UINT32(colo_requested, COLOInfo),
         VMSTATE_END_OF_LIST()
        },
};

void colo_info_mig_init(void)
{
    vmstate_register(NULL, 0, &colo_state, &colo_info);
}

bool loadvm_enable_colo(void)
{
    return colo_info.colo_requested;
}

void loadvm_exit_colo(void)
{
    colo_info.colo_requested = 0;
}
