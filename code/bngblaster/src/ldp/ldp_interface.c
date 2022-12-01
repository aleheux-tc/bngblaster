/*
 * BNG Blaster (BBL) - LDP Interface
 * 
 * Christian Giese, November 2022
 *
 * Copyright (C) 2020-2022, RtBrick, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "ldp.h"

void
ldp_interface_hello_job(timer_s *timer)
{
    ldp_adjacency_s *adjacency = timer->data;
    bbl_network_interface_s *interface = adjacency->interface;

    interface->send_requests |= BBL_IF_SEND_LDP_HELLO;
}

/**
 * ldp_interface_init
 * 
 * This function inits the LDP interface. 
 *
 * @param interface network interface
 * @param config network interface configuration
 * @param instance LDP instance
 */
bool 
ldp_interface_init(bbl_network_interface_s *interface,
                   bbl_network_config_s *interface_config,
                   ldp_instance_s *instance)
{
    ldp_config_s *config = instance->config;
    ldp_adjacency_s *adjacency;

    LOG(LDP, "Add network interface %s to LDP instance %u\n", 
        interface->name, interface_config->ldp_instance_id);

    adjacency = calloc(1, sizeof(ldp_adjacency_s));
    adjacency->next = instance->adjacencies;
    instance->adjacencies = adjacency;
    adjacency->instance = instance;
    adjacency->interface = interface;
    interface->ldp_adjacency = adjacency;
    
    ldp_hello_start(config, adjacency);
    return true;
}