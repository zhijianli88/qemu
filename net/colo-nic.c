/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO.,LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <libnfnetlink/libnfnetlink.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "include/migration/migration.h"
#include "migration/migration-colo.h"
#include "net/net.h"
#include "net/colo-nic.h"
#include "qemu/error-report.h"
#include "trace.h"

/* Remove the follow define after proxy is merged into kernel,
* using #include <libnfnetlink/libnfnetlink.h> instead.
*/
#define NFNL_SUBSYS_COLO 12

/* Message Format
* <---NLMSG_ALIGN(hlen)-----><-------------- NLMSG_ALIGN(len)----------------->
* +--------------------+- - -+- - - - - - - - - - - - - - +- - - - - - + - - -+
* |       Header       | Pad |   Netfilter Netlink Header | Attributes | Pad  |
* |    struct nlmsghdr |     |     struct nfgenmsg        |            |      |
* +--------------------+- - -+- - - - - - - - - - - - - - + - - - - - -+ - - -+
*/

enum nfnl_colo_msg_types {
    NFCOLO_KERNEL_NOTIFY, /* Used by proxy module to notify qemu */

    NFCOLO_DO_CHECKPOINT,
    NFCOLO_DO_FAILOVER,
    NFCOLO_PROXY_INIT,
    NFCOLO_PROXY_RESET,

    NFCOLO_MSG_MAX
};

enum nfnl_colo_kernel_notify_attributes {
    NFNL_COLO_KERNEL_NOTIFY_UNSPEC,
    NFNL_COLO_COMPARE_RESULT,
    __NFNL_COLO_KERNEL_NOTIFY_MAX
};

#define NFNL_COLO_KERNEL_NOTIFY_MAX  (__NFNL_COLO_KERNEL_NOTIFY_MAX - 1)

enum nfnl_colo_attributes {
    NFNL_COLO_UNSPEC,
    NFNL_COLO_MODE,
    __NFNL_COLO_MAX
};
#define NFNL_COLO_MAX  (__NFNL_COLO_MAX - 1)

struct nfcolo_msg_mode {
    u_int8_t mode;
};

struct nfcolo_packet_compare { /* Unused */
    int32_t different;
};

typedef struct nic_device {
    NetClientState *nc;
    bool (*support_colo)(NetClientState *nc);
    int (*configure)(NetClientState *nc, bool up, int side, int index);
    QTAILQ_ENTRY(nic_device) next;
    bool is_up;
} nic_device;

static struct nfnl_handle *nfnlh;
static struct nfnl_subsys_handle *nfnlssh;
static int32_t packet_compare_different; /* The result of packet comparing */

QTAILQ_HEAD(, nic_device) nic_devices = QTAILQ_HEAD_INITIALIZER(nic_devices);

/*
* colo_proxy_script usage
* ./colo_proxy_script master/slave install/uninstall phy_if virt_if index
*/
static bool colo_nic_support(NetClientState *nc)
{
    return nc && nc->colo_script[0] && nc->colo_nicname[0];
}

static int launch_colo_script(char *argv[])
{
    int pid, status;
    char *script = argv[0];

    /* try to launch network script */
    pid = fork();
    if (pid == 0) {
        execv(script, argv);
        _exit(1);
    } else if (pid > 0) {
        while (waitpid(pid, &status, 0) != pid) {
            /* loop */
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0;
        }
    }
    return -1;
}

static int colo_nic_configure(NetClientState *nc,
            bool up, int side, int index)
{
    int i, argc = 6;
    char *argv[7], index_str[32];
    char **parg;

    if (!nc && index <= 0) {
        error_report("Can not parse colo_script or colo_nicname");
        return -1;
    }

    parg = argv;
    *parg++ = nc->colo_script;
    *parg++ = (char *)(side == COLO_SECONDARY_MODE ? "slave" : "master");
    *parg++ = (char *)(up ? "install" : "uninstall");
    *parg++ = nc->colo_nicname;
    *parg++ = nc->ifname;
    sprintf(index_str, "%d", index);
    *parg++ = index_str;
    *parg = NULL;

    for (i = 0; i < argc; i++) {
        if (!argv[i][0]) {
            error_report("Can not get colo_script argument");
            return -1;
        }
    }

    return launch_colo_script(argv);
}

static int configure_one_nic(NetClientState *nc,
             bool up, int side, int index)
{
    struct nic_device *nic;

    assert(nc);

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        if (nic->nc == nc) {
            if (!nic->support_colo || !nic->support_colo(nic->nc)
                || !nic->configure) {
                return -1;
            }
            if (up == nic->is_up) {
                return 0;
            }

            if (nic->configure(nic->nc, up, side, index) && up) {
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
        if (configure_one_nic(nic->nc, 1, side, index)) {
            return -1;
        }
    }

    return 0;
}

static void teardown_nic(int side, int index)
{
    struct nic_device *nic;

    QTAILQ_FOREACH(nic, &nic_devices, next) {
        configure_one_nic(nic->nc, 0, side, index);
    }
}

void colo_add_nic_devices(NetClientState *nc)
{
    struct nic_device *nic = g_malloc0(sizeof(*nic));

    nic->support_colo = colo_nic_support;
    nic->configure = colo_nic_configure;
    /*
     * TODO
     * only support "-netdev tap,colo_scripte..."  options
     * "-net nic -net tap..." options is not supported
     */
    nic->nc = nc;

    QTAILQ_INSERT_TAIL(&nic_devices, nic, next);
}

void colo_remove_nic_devices(NetClientState *nc)
{
    struct nic_device *nic, *next_nic;

    if (!nc) {
        return;
    }

    QTAILQ_FOREACH_SAFE(nic, &nic_devices, next, next_nic) {
        if (nic->nc == nc) {
            configure_one_nic(nc, 0, get_colo_mode(), getpid());
            QTAILQ_REMOVE(&nic_devices, nic, next);
            g_free(nic);
        }
    }
}

static int colo_proxy_send(enum nfnl_colo_msg_types msg_type,
                           enum colo_mode mode, int flag, void *unused)
{
    struct nfcolo_msg_mode params;
    union {
        char buf[NFNL_HEADER_LEN
                 + NFA_LENGTH(sizeof(struct nfcolo_msg_mode))];
        struct nlmsghdr nmh;
    } u;
    int ret;

    if (!nfnlssh || !nfnlh) {
        error_report("nfnlssh and nfnlh are uninited");
        return -1;
    }
    nfnl_fill_hdr(nfnlssh, &u.nmh, 0, AF_UNSPEC, 1,
                  msg_type, NLM_F_REQUEST | flag);
    params.mode = mode;
    u.nmh.nlmsg_pid = nfnl_portid(nfnlh);
    ret = nfnl_addattr_l(&u.nmh, sizeof(u),  NFNL_COLO_MODE, &params,
                         sizeof(params));
    if (ret < 0) {
        error_report("call nfnl_addattr_l failed");
        return ret;
    }
    ret = nfnl_send(nfnlh, &u.nmh);
    if (ret < 0) {
        error_report("call nfnl_send failed");
    }
    return ret;
}

static int __colo_rcv_pkt(struct nlmsghdr *nlh, struct nfattr *nfa[],
                          void *data)
{
    /* struct nfgenmsg *nfmsg = NLMSG_DATA(nlh); */
    int32_t  result = ntohl(nfnl_get_data(nfa, NFNL_COLO_COMPARE_RESULT,
                                          int32_t));

    atomic_set(&packet_compare_different, result);
    trace_colo_rcv_pkt(result);
    return 0;
}

static struct nfnl_callback colo_nic_cb = {
    .call   = &__colo_rcv_pkt,
    .attr_count = NFNL_COLO_KERNEL_NOTIFY_MAX,
};

static void colo_proxy_recv(void *opaque)
{
    unsigned char *buf = g_malloc0(2048);
    int len;
    int ret;

    len = nfnl_recv(nfnlh, buf, 2048);
    ret = nfnl_handle_packet(nfnlh, (char *)buf, len);
    if (ret < 0) {/* Notify colo thread the error */
        atomic_set(&packet_compare_different, -1);
        error_report("call nfnl_handle_packet failed");
    }
    g_free(buf);
}

static int check_proxy_ack(void)
{
    unsigned char *buf = g_malloc0(2048);
    struct nlmsghdr *nlmsg;
    int len;
    int ret = -1;

    len = nfnl_recv(nfnlh, buf, 2048);
    if (len <= 0) {
        error_report("nfnl_recv received nothing");
        goto err;
    }
    nlmsg = (struct nlmsghdr *)buf;

    if (nlmsg->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlmsg);

        if (err->error) {
            error_report("Received error message:%d",  -err->error);
            goto err;
        }
    }

    ret = 0;
err:
    g_free(buf);
    return ret;
}

int colo_proxy_init(enum colo_mode mode)
{
    int ret = -1;

    nfnlh = nfnl_open();
    if (!nfnlh) {
        error_report("call nfnl_open failed");
        return -1;
    }
    /* Note:
     *  Here we must ensure that the nl_pid (also nlmsg_pid in nlmsghdr ) equal
     *  to the process ID of VM, becase we use it to identify the VM in proxy
     *  module.
     */
    if (nfnl_portid(nfnlh) != getpid()) {
        error_report("More than one netlink of NETLINK_NETFILTER type exist");
        return -1;
    }
    /* disable netlink sequence tracking by default */
    nfnl_unset_sequence_tracking(nfnlh);
    nfnlssh = nfnl_subsys_open(nfnlh, NFNL_SUBSYS_COLO, NFCOLO_MSG_MAX, 0);
    if (!nfnlssh) {
        error_report("call nfnl_subsys_open failed");
        goto err_out;
    }

    ret = nfnl_callback_register(nfnlssh, NFCOLO_KERNEL_NOTIFY, &colo_nic_cb);
    if (ret < 0) {
        goto err_out;
    }

    /* Netlink is not a reliable protocol, So it is necessary to request proxy
     * module to acknowledge in the first time.
     */
    ret = colo_proxy_send(NFCOLO_PROXY_INIT, mode, NLM_F_ACK, NULL);
    if (ret < 0) {
        goto err_out;
    }

    ret = check_proxy_ack();
    if (ret < 0) {
        goto err_out;
    }

    ret = configure_nic(mode, getpid());
    if (ret != 0) {
        error_report("excute colo-proxy-script failed");
        goto err_out;
    }

   qemu_set_fd_handler(nfnl_fd(nfnlh), colo_proxy_recv, NULL, NULL);

    return 0;
err_out:
    nfnl_close(nfnlh);
    return ret;
}

void colo_proxy_destroy(enum colo_mode mode)
{
    nfnl_close(nfnlh);
    teardown_nic(mode, getpid());
}

/*
* Note: Weird, Only the VM in slave side need to do failover work !!!
*/
int colo_proxy_failover(void)
{
    if (colo_proxy_send(NFCOLO_DO_FAILOVER, COLO_SECONDARY_MODE, 0, NULL) < 0) {
        return -1;
    }

    return 0;
}

/*
* Note: Only the VM in master side need to do checkpoint
*/
int colo_proxy_checkpoint(enum colo_mode  mode)
{
    if (colo_proxy_send(NFCOLO_DO_CHECKPOINT, mode, 0, NULL) < 0) {
        return -1;
    }
    return 0;
}

int colo_proxy_compare(void)
{
    return atomic_xchg(&packet_compare_different, 0);
}
