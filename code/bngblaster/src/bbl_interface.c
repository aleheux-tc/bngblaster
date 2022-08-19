/*
 * BNG Blaster (BBL) - Interfaces
 *
 * Christian Giese, October 2020
 *
 * Copyright (C) 2020-2022, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bbl.h"
#include "bbl_io.h"
#include <sys/stat.h>

void
bbl_interface_rate_job(timer_s *timer) {
    bbl_interface_s *interface = timer->data;
    bbl_compute_avg_rate(&interface->stats.rate_packets_tx, interface->stats.packets_tx);
    bbl_compute_avg_rate(&interface->stats.rate_packets_rx, interface->stats.packets_rx);
    bbl_compute_avg_rate(&interface->stats.rate_bytes_tx, interface->stats.bytes_tx);
    bbl_compute_avg_rate(&interface->stats.rate_bytes_rx, interface->stats.bytes_rx);
}

/**
 * bbl_interface_lock
 *
 * @brief This functions locks the interface
 * creating the file "/run/lock/bngblaster_<interface>.lock".
 *
 * @param interface interface
 * @return false if failed to lock (e.g. in use)
 */
static bool
bbl_interface_lock(char *interface_name)
{
    FILE *lock_file;
    char  lock_path[FILE_PATH_LEN];
    int   lock_pid;
    char  proc_pid_path[FILE_PATH_LEN];

    struct stat sts;
    pid_t pid = getpid();

    snprintf(lock_path, sizeof(lock_path), "/run/lock/bngblaster_%s.lock", interface_name);
    lock_file = fopen(lock_path, "r");
    if(lock_file) {
        /* lock file exists */
        if(fscanf(lock_file,"%d", &lock_pid) == 1 && lock_pid > 1) {
            snprintf(proc_pid_path, sizeof(proc_pid_path), "/proc/%d", lock_pid);
            if (!(stat(proc_pid_path, &sts) == -1 && errno == ENOENT)) {
                LOG(ERROR, "Interface %s in use by process %d (%s)\n", interface_name, lock_pid, lock_path);
                if(!g_ctx->config.interface_lock_force) return false;
            }
        } else {
            LOG(ERROR, "Invalid interface lock file %s\n", lock_path);
            if(!g_ctx->config.interface_lock_force) return false;
        }
        fclose(lock_file);
    }
    /* crate lock file */
    lock_pid = pid;
    lock_file = fopen(lock_path, "w");
    if(!lock_file) {
        LOG(ERROR, "Failed to open interface lock file %s %s (%d)\n", 
            lock_path, strerror(errno), errno);
        return false;
    }
    fprintf(lock_file, "%d", lock_pid);
    fclose(lock_file);
    return true;
}

/**
 * bbl_interface_unlock_all
 *
 * @brief This functions unlocks all interfaces.
 */
void
bbl_interface_unlock_all()
{
    char lock_path[FILE_PATH_LEN];
    struct bbl_interface_ *interface;
    CIRCLEQ_FOREACH(interface, &g_ctx->interface_qhead, interface_qnode) {
        snprintf(lock_path, sizeof(lock_path), "/run/lock/bngblaster_%s.lock", interface->name);
        remove(lock_path);
    }
}

static bool
bbl_interface_set_kernel_info(bbl_interface_s *interface)
{
    struct ifreq ifr = {0};

    if(interface->io.mode == IO_MODE_DPDK) {
        /* This will not work for DPDK bound interfaces. */
        return true;
    }

    int fd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", interface->name);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
        LOG(ERROR, "Getting MAC address error %s (%d) for interface %s\n",
            strerror(errno), errno, interface->name);
        close(fd);
        return false;
    }
    memcpy(&interface->mac, ifr.ifr_hwaddr.sa_data, IFHWADDRLEN);

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", interface->name);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) == -1) {
        LOG(ERROR, "Get interface index error %s (%d) for interface %s\n",
            strerror(errno), errno, interface->name);
        close(fd);
        return false;
    }
    interface->ifindex = ifr.ifr_ifindex;

    close(fd);
    return true;
}

/**
 * bbl_add_interface
 *
 * @param interface interface name
 * @param link_config optional link configuration
 * @return interface
 */
static bbl_interface_s *
bbl_interface_add(char *interface_name, bbl_link_config_s *link_config)
{
    bbl_interface_s *interface;

    interface = calloc(1, sizeof(bbl_interface_s));
    if (!interface) {
        LOG(ERROR, "No memory for interface %s\n", interface_name);
        return NULL;
    }
    interface->name = strdup(interface_name);
    interface->pcap_index = g_ctx->pcap.index++;
    
    if(!bbl_interface_lock(interface_name)) {
        return NULL;
    }
    CIRCLEQ_INSERT_TAIL(&g_ctx->interface_qhead, interface, interface_qnode);

    if(!bbl_interface_set_kernel_info(interface)) {
        return NULL;
    }

    interface->config = link_config;
    interface->io.rx_buf = malloc(IO_BUFFER_LEN);
    interface->io.tx_buf = malloc(IO_BUFFER_LEN);
    interface->io.mode = link_config->io_mode;
    if(*(uint64_t*)link_config->mac & 0xffffffffffff00) {
        memcpy(interface->mac, link_config->mac, ETH_ADDR_LEN);
    }
    if(!bbl_lag_interface_add(interface, link_config)) {
        return NULL;
    }

    /* The BNG Blaster supports multiple IO modes where packet_mmap is
     * selected per default. */
    if(!bbl_io_add_interface(interface)) {
        return NULL;
    }

    /*
     * Timer to compute periodic rates.
     */
    timer_add_periodic(&g_ctx->timer_root, &interface->rate_job, "Rate Computation", 1, 0, interface,
                       &bbl_interface_rate_job);
    return interface;
}

/**
 * bbl_interface_get
 * 
 * Get interface by name. 
 *
 * @param interface_name interface name
 * @return the interface or NULL
 */
bbl_interface_s *
bbl_interface_get(char *interface_name)
{
    bbl_interface_s *interface;
    CIRCLEQ_FOREACH(interface, &g_ctx->interface_qhead, interface_qnode) {
        if (strcmp(interface->name, interface_name) == 0) {
            return interface;
        }
    }
    return NULL;
}

/**
 * bbl_interface_init
 */
static bool
bbl_interface_init()
{
    bbl_link_config_s *link_config = g_ctx->config.link_config;
    struct bbl_interface_ *interface;

    while(link_config) {
        if(bbl_interface_get(link_config->interface) != NULL) {
            LOG(ERROR, "Failed to add link %s (duplicate link configuration)\n", 
                link_config->interface);
            return false;
        }
        interface = bbl_interface_add(link_config->interface, link_config);
        if (!interface) {
            LOG(ERROR, "Failed to add link %s\n", link_config->interface);
            return false;
        }
        link_config = link_config->next;
    }
    return true;
}

/**
 * bbl_interface_init
 *
 * @brief This function will add and initialize
 * all interfaces defined in the configuration.
 *
 * @return true if all interfaces are
 * added and initialised successfully
 */
bool
bbl_interface_init()
{
    /* LAG must be added before links, so that links
     * can reference to LAG. */
    if(!bbl_lag_add()) {
        return false;
    }
    if(!bbl_interface_links_add()) {
        return false;
    }
    if(!bbl_access_interfaces_add()) {
        return false;
    }
    if(!bbl_network_interfaces_add()) {
        return false;
    }
    if(!bbl_a10nsp_add()) {
        return false;
    }
    return true;
}