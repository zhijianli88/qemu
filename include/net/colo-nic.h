/*
 *  COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 *  (a.k.a. Fault Tolerance or Continuous Replication)
 *
 *  Copyright (C) 2014 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#ifndef COLO_NIC_H
#define COLO_NIC_H

void colo_add_nic_devices(NetClientState *nc);
void colo_remove_nic_devices(NetClientState *nc);

#endif
