/* Copyright (C) 2015 Hewlett Packard Enterprise Development LP
 * All Rights Reserved.

 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

/* Purpose: This file contains code to enable routing related functionality
 * in the Broadcom ASIC.
 */

#include <string.h>
#include <errno.h>
#include <assert.h>
#include <util.h>
#include <netinet/ether.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <openvswitch/vlog.h>
#include <opennsl/error.h>
#include <opennsl/types.h>
#include <opennsl/switch.h>
#include <opennsl/vlan.h>
#include <opennsl/l3.h>
#include <opennsl/l2.h>
#include <ofproto/ofproto.h>
#include "ops-routing.h"
#include "ops-debug.h"
#include "ops-vlan.h"
#include "ops-knet.h"
#include "platform-defines.h"
#include "openhalon-dflt.h"


VLOG_DEFINE_THIS_MODULE(ops_routing);

opennsl_if_t local_nhid;
/* fake MAC to create a local_nhid */
opennsl_mac_t LOCAL_MAC =  {0x0,0x0,0x01,0x02,0x03,0x04};

/* all routes in asic*/
struct ops_route_table{
   struct hmap routes;
};

struct ops_route_table ops_rtable;

int
ops_l3_init(int unit)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_l3_egress_t egress_object;

    rc = opennsl_switch_control_set(unit, opennslSwitchL3IngressMode, 1);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchL3IngressMode: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    rc = opennsl_switch_control_set(unit, opennslSwitchL3EgressMode, 1);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchL3EgressMode: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    /* Create a system wide egress object for unresolved NH */
    opennsl_l3_egress_t_init(&egress_object);

    egress_object.intf = -1;
    egress_object.port = 0; /* CPU port */
    egress_object.flags = OPENNSL_L3_COPY_TO_CPU;
    memcpy(egress_object.mac_addr, LOCAL_MAC, ETH_ALEN);
    rc = opennsl_l3_egress_create(unit, OPENNSL_L3_COPY_TO_CPU,
                                  &egress_object, &local_nhid);

    if (rc != OPENNSL_E_NONE) {
        VLOG_ERR("Error, create a local egress object, rc=%d", rc);
        return rc;
    }

    /* Send ARP to CPU */
    rc = opennsl_switch_control_set(unit, opennslSwitchArpRequestToCpu, 1);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchArpRequestToCpu: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    rc = opennsl_switch_control_set(unit, opennslSwitchArpReplyToCpu, 1);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchArpReplyToCpu: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    /* IPv6 ND packets */
    rc = opennsl_switch_control_set(unit, opennslSwitchNdPktToCpu, 1);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchNdPktToCpu: unit=%d  rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    /* Send IPv4 and IPv6 to CPU */
    rc = opennsl_switch_control_set(unit,opennslSwitchUnknownL3DestToCpu, 1);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchUnknownL3DestToCpu: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    rc = opennsl_switch_control_set(unit, opennslSwitchV6L3DstMissToCpu, 1);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchV6L3DstMissToCpu: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    /* Enable ECMP enhanced hash method */
    rc = opennsl_switch_control_set(unit, opennslSwitchHashControl,
                                    OPENNSL_HASH_CONTROL_ECMP_ENHANCE);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set OPENNSL_HASH_CONTROL_ECMP_ENHANCE: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    /* Enable IPv4 src ip, src port, dst ip, dst port hashing by default */
    rc = opennsl_switch_control_set(unit, opennslSwitchHashIP4Field0,
                                    OPENNSL_HASH_FIELD_IP4SRC_LO |
                                    OPENNSL_HASH_FIELD_IP4SRC_HI |
                                    OPENNSL_HASH_FIELD_SRCL4     |
                                    OPENNSL_HASH_FIELD_IP4DST_LO |
                                    OPENNSL_HASH_FIELD_IP4DST_HI |
                                    OPENNSL_HASH_FIELD_DSTL4);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchHashIP4Field0: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    /* Enable IPv6 src ip, src port, dst ip, dst port hashing by default */
    rc = opennsl_switch_control_set(unit, opennslSwitchHashIP6Field0,
                                    OPENNSL_HASH_FIELD_IP6SRC_LO |
                                    OPENNSL_HASH_FIELD_IP6SRC_HI |
                                    OPENNSL_HASH_FIELD_SRCL4     |
                                    OPENNSL_HASH_FIELD_IP6DST_LO |
                                    OPENNSL_HASH_FIELD_IP6DST_HI |
                                    OPENNSL_HASH_FIELD_DSTL4);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchHashIP6Field0: unit=%d rc=%s",
                 unit, opennsl_errmsg(rc));
        return 1;
    }

    /* initialize route table hash map */
    hmap_init(&ops_rtable.routes);

    return 0;
}

opennsl_l3_intf_t *
ops_routing_enable_l3_interface(int hw_unit, opennsl_port_t hw_port,
                                opennsl_vrf_t vrf_id, opennsl_vlan_t vlan_id,
                                unsigned char *mac)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_pbmp_t pbmp;
    opennsl_l3_intf_t *l3_intf;

    /* VLAN config */
    rc = bcmsdk_create_vlan(vlan_id, true);
    if (rc < 0) {
        VLOG_ERR("Failed at bcmsdk_create_vlan: unit=%d port=%d vlan=%d rc=%d",
                 hw_unit, hw_port, vlan_id, rc);
        goto failed_vlan_creation;
    }


    OPENNSL_PBMP_CLEAR(pbmp);
    OPENNSL_PBMP_PORT_ADD(pbmp, hw_port);
    rc = bcmsdk_add_access_ports(vlan_id, &pbmp, true);
    if (rc < 0) {
        VLOG_ERR("Failed at bcmsdk_add_access_ports: unit=%d port=%d vlan=%d rc=%d",
                 hw_unit, hw_port, vlan_id, rc);
        goto failed_adding_vlan;
    }

    /* Create L3 interface */
    l3_intf = (opennsl_l3_intf_t *)xmalloc(sizeof(opennsl_l3_intf_t));
    if (!l3_intf) {
        VLOG_ERR("Failed allocating opennsl_l3_intf_t: unit=%d port=%d vlan=%d rc=%d",
                 hw_unit, hw_port, vlan_id, rc);
        goto failed_allocating_l3_intf;
    }

    opennsl_l3_intf_t_init(l3_intf);
    l3_intf->l3a_vrf = vrf_id;
    l3_intf->l3a_intf_id = vlan_id;
    l3_intf->l3a_flags = OPENNSL_L3_ADD_TO_ARL | OPENNSL_L3_WITH_ID;
    memcpy(l3_intf->l3a_mac_addr, mac, ETH_ALEN);
    l3_intf->l3a_vid = vlan_id;

    rc = opennsl_l3_intf_create(hw_unit, l3_intf);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed at opennsl_l3_intf_create: unit=%d port=%d vlan=%d vrf=%d rc=%s",
                 hw_unit, hw_port, vlan_id, vrf_id, opennsl_errmsg(rc));
        goto failed_l3_intf_create;
    }

    SW_L3_DBG("Enabled L3 on unit=%d port=%d vlan=%d vrf=%d",
            hw_unit, hw_port, vlan_id, vrf_id);

    return l3_intf;

failed_l3_intf_create:
    free(l3_intf);

failed_allocating_l3_intf:
    OPENNSL_PBMP_CLEAR(pbmp);
    OPENNSL_PBMP_PORT_ADD(pbmp, hw_port);
    rc = bcmsdk_del_access_ports(vlan_id, &pbmp, true);
    if (rc < 0) {
        VLOG_ERR("Failed at bcmsdk_del_access_ports: unit=%d port=%d rc=%d",
                 hw_unit, hw_port, rc);
    }

failed_adding_vlan:
    rc = bcmsdk_destroy_vlan(vlan_id, true);
    if (rc < 0) {
        VLOG_ERR("Failed at bcmsdk_destroy_vlan: unit=%d port=%d vlan=%d rc=%d",
                 hw_unit, hw_port, vlan_id, rc);
    }

failed_vlan_creation:
    return NULL;
}

void
ops_routing_disable_l3_interface(int hw_unit, opennsl_port_t hw_port,
                                 opennsl_l3_intf_t *l3_intf)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_vlan_t vlan_id = l3_intf->l3a_vid;
    opennsl_vrf_t vrf_id = l3_intf->l3a_vrf;
    opennsl_pbmp_t pbmp;


    rc = opennsl_l3_intf_delete(hw_unit, l3_intf);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed at opennsl_l3_intf_delete: unit=%d port=%d vlan=%d vrf=%d rc=%s",
                 hw_unit, hw_port, vlan_id, vrf_id, opennsl_errmsg(rc));
    }

    /* Reset VLAN on port back to default and destroy the VLAN */
    OPENNSL_PBMP_CLEAR(pbmp);
    OPENNSL_PBMP_PORT_ADD(pbmp, hw_port);
    rc = bcmsdk_del_access_ports(vlan_id, &pbmp, true);
    if (rc < 0) {
        VLOG_ERR("Failed at bcmsdk_del_access_ports: unit=%d port=%d rc=%d",
                 hw_unit, hw_port, rc);
    }

    rc = bcmsdk_destroy_vlan(vlan_id, true);
    if (rc < 0) {
        VLOG_ERR("Failed at bcmsdk_destroy_vlan: unit=%d port=%d vlan=%d rc=%d",
                 hw_unit, hw_port, vlan_id, rc);
    }

    SW_L3_DBG("Disabled L3 on unit=%d port=%d vrf=%d", hw_unit, hw_port, vrf_id);
}

opennsl_l3_intf_t *
ops_routing_enable_l3_vlan_interface(int hw_unit, opennsl_vrf_t vrf_id,
                                     opennsl_vlan_t vlan_id,
                                     unsigned char *mac)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_l3_intf_t *l3_intf;

    /* Create L3 interface */
    l3_intf = (opennsl_l3_intf_t *)xmalloc(sizeof(opennsl_l3_intf_t));
    if (!l3_intf) {
        VLOG_ERR("Failed allocating opennsl_l3_intf_t: unit=%d vlan=%d rc=%d",
                 hw_unit, vlan_id, rc);
        return NULL;
    }

    opennsl_l3_intf_t_init(l3_intf);
    l3_intf->l3a_vrf = vrf_id;
    l3_intf->l3a_intf_id = vlan_id;
    l3_intf->l3a_flags = OPENNSL_L3_ADD_TO_ARL | OPENNSL_L3_WITH_ID;
    memcpy(l3_intf->l3a_mac_addr, mac, ETH_ALEN);
    l3_intf->l3a_vid = vlan_id;

    rc = opennsl_l3_intf_create(hw_unit, l3_intf);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed at opennsl_l3_intf_create: unit=%d vlan=%d vrf=%d rc=%s",
                 hw_unit, vlan_id, vrf_id, opennsl_errmsg(rc));
        free(l3_intf);
        return NULL;
    }

    SW_L3_DBG("Enabled L3 on unit=%d vlan=%d vrf=%d",
            hw_unit, vlan_id, vrf_id);

    return l3_intf;
} /* ops_routing_enable_l3_vlan_interface */

/* Add nexthop into the route entry */
static void
ops_nexthop_add(struct ops_route *route,  struct ofproto_route_nexthop *of_nh)
{
    char *hashstr;
    struct ops_nexthop *nh;

    if (!route || !of_nh) {
        return;
    }

    nh = xzalloc(sizeof(*nh));
    nh->type = of_nh->type;
    /* NOTE: Either IP or Port, not both */
    if (of_nh->id) {
        nh->id = xstrdup(of_nh->id);
    }

    nh->l3_egress_id = (of_nh->state == OFPROTO_NH_RESOLVED) ?
                        of_nh->l3_egress_id : local_nhid ;

    hashstr = of_nh->id;
    hmap_insert(&route->nexthops, &nh->node, hash_string(hashstr, 0));
    route->n_nexthops++;

    VLOG_DBG("Add NH %s, egress_id %d, for route %s",
              nh->id, nh->l3_egress_id, route->prefix);
} /* ops_nexthop_add */

/* Delete nexthop into route entry */
static void
ops_nexthop_delete(struct ops_route *route, struct ops_nexthop *nh)
{
    if (!route || !nh) {
        return;
    }

    VLOG_DBG("Delete NH %s in route %s", nh->id, route->prefix);

    hmap_remove(&route->nexthops, &nh->node);
    if (nh->id) {
        free(nh->id);
    }
    free(nh);
    route->n_nexthops--;
} /* ops_nexthop_delete */

/* Find nexthop entry in the route's nexthops hash */
static struct ops_nexthop*
ops_nexthop_lookup(struct ops_route *route, struct ofproto_route_nexthop *of_nh)
{
    char *hashstr;
    struct ops_nexthop *nh;

    hashstr = of_nh->id;
    HMAP_FOR_EACH_WITH_HASH(nh, node, hash_string(hashstr, 0),
                            &route->nexthops) {
        if ((strcmp(nh->id, of_nh->id) == 0)){
            return nh;
        }
    }
    return NULL;
} /* ops_nexthop_lookup */

/* Create route hash */
static void
ops_route_hash(int vrf, char *prefix, char *hashstr, int hashlen)
{
    snprintf(hashstr, hashlen, "%d:%s", vrf, prefix);
} /* ops_route_hash */

/* Find a route entry matching the prefix */
static struct ops_route *
ops_route_lookup(int vrf, struct ofproto_route *of_routep)
{
    struct ops_route *route;
    char hashstr[OPS_ROUTE_HASH_MAXSIZE];

    ops_route_hash(vrf, of_routep->prefix, hashstr, sizeof(hashstr));
    HMAP_FOR_EACH_WITH_HASH(route, node, hash_string(hashstr, 0),
                            &ops_rtable.routes) {
        if ((strcmp(route->prefix, of_routep->prefix) == 0) &&
            (route->vrf == vrf)) {
            return route;
        }
    }
    return NULL;
} /* ops_route_lookup */

/* Add new route and NHs */
static struct ops_route*
ops_route_add(int vrf, struct ofproto_route *of_routep)
{
    int i;
    struct ops_route *routep;
    struct ofproto_route_nexthop *of_nh;
    char hashstr[OPS_ROUTE_HASH_MAXSIZE];

    if (!of_routep) {
        return NULL;
    }

    routep = xzalloc(sizeof(*routep));
    routep->vrf = vrf;
    routep->prefix = xstrdup(of_routep->prefix);
    routep->is_ipv6 = (of_routep->family == OFPROTO_ROUTE_IPV6) ? true : false;
    routep->n_nexthops = 0;

    hmap_init(&routep->nexthops);

    for (i = 0; i < of_routep->n_nexthops; i++) {
        of_nh = &of_routep->nexthops[i];
        ops_nexthop_add(routep, of_nh);
    }

    ops_route_hash(vrf, of_routep->prefix, hashstr, sizeof(hashstr));
    hmap_insert(&ops_rtable.routes, &routep->node, hash_string(hashstr, 0));
    VLOG_DBG("Add route %s", of_routep->prefix);
    return routep;
} /* ops_route_add */

/* Update route nexthop: add, delete, resolve and unresolve nh */
static void
ops_route_update(int vrf, struct ops_route *routep,
                 struct ofproto_route *of_routep,
                 bool is_delete_nh)
{
    struct ops_nexthop* nh;
    struct ofproto_route_nexthop *of_nh;
    int i;

    for (i = 0; i < of_routep->n_nexthops; i++) {
        of_nh = &of_routep->nexthops[i];
        nh = ops_nexthop_lookup(routep, of_nh);
        if (is_delete_nh) {
            ops_nexthop_delete(routep, nh);
        } else {
            /* add or update */
            if (!nh) {
                ops_nexthop_add(routep, of_nh);
            } else {
                /* update is currently resolved on unreoslved */
                nh->l3_egress_id = (of_nh->state == OFPROTO_NH_RESOLVED) ?
                                    of_nh->l3_egress_id : local_nhid ;
            }
        }
    }
} /* ops_route_update */

/* Delete route in system*/
static void
ops_route_delete(struct ops_route *routep)
{
    struct ops_nexthop *nh, *next;

    if (!routep) {
        return;
    }

    VLOG_DBG("delete route %s", routep->prefix);

    hmap_remove(&ops_rtable.routes, &routep->node);

    HMAP_FOR_EACH_SAFE(nh, next, node, &routep->nexthops) {
        ops_nexthop_delete(routep, nh);
    }

    if (routep->prefix) {
        free(routep->prefix);
    }
    free(routep);
} /* ops_route_delete */

/* Function to add l3 host entry via ofproto */
int
ops_routing_add_host_entry(int hw_unit, opennsl_port_t hw_port,
                           opennsl_vrf_t vrf_id, bool is_ipv6_addr,
                           char *ip_addr, char *next_hop_mac_addr,
                           opennsl_if_t l3_intf_id,
                           opennsl_if_t *l3_egress_id,
                           opennsl_vlan_t vlan_id)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_l3_egress_t egress_object;
    opennsl_l3_host_t l3host;
    in_addr_t ipv4_dest_addr;
    char ipv6_dest_addr[sizeof(struct in6_addr)];
    int flags = 0;
    struct ether_addr *ether_mac = ether_aton(next_hop_mac_addr);
    opennsl_port_t port = hw_port;
    opennsl_l2_addr_t addr;

    /* If we dont have a hw_port, this is likely a vlan interface
     * Look it up.
     */
    if(hw_port == -1) {
        opennsl_mac_t host_mac;
        if (ether_mac != NULL)
           memcpy(host_mac, ether_mac, ETH_ALEN);
        opennsl_l2_addr_get(hw_unit, host_mac, vlan_id, &addr);
        port = addr.port;
    }

    /* Create the l3_egress object which gives the index to l3 interface
     * during lookup */
    VLOG_DBG("In ops_routing_add_host_entry for ip %s", ip_addr);
    opennsl_l3_egress_t_init(&egress_object);

    /* Copy the nexthop destmac, set dest port and index of L3_INTF table
     * which is created above */
    egress_object.intf = l3_intf_id;
    egress_object.port = port;

    if (ether_mac != NULL) {
        memcpy(egress_object.mac_addr, ether_mac, ETH_ALEN);
    } else {
        VLOG_ERR("Invalid mac-%s", next_hop_mac_addr);
        return 1; /* Return error */
    }

    rc = opennsl_l3_egress_create(hw_unit, flags, &egress_object, l3_egress_id);
    if (rc != OPENNSL_E_NONE) {
        VLOG_ERR("Error, create egress object, out_port=%d", hw_port);
        return rc;
    }

    VLOG_DBG("Created L3 egress ID %d for out_port: %d intf_id: %d ",
          *l3_egress_id, port, l3_intf_id);

    /* Create Host Entry */
    opennsl_l3_host_t_init(&l3host);
    if( is_ipv6_addr ) {
        flags |= OPENNSL_L3_IP6;

        /* convert string ip into host format */
        rc = inet_pton(AF_INET6, ip_addr, ipv6_dest_addr);
        if ( rc != 1 ) {
            VLOG_ERR("Failed to create L3 host entry. Invalid ipv6 address %s", ip_addr);
            return 1; /* Return error */
        }

        memcpy(l3host.l3a_ip6_addr, ipv6_dest_addr, sizeof(struct in6_addr));
    } else {
        /* convert string ip into host format */
        ipv4_dest_addr = inet_network(ip_addr);
        if ( ipv4_dest_addr == -1 ) {
            VLOG_ERR("Failed to create L3 host entry. Invalid ipv4 address %s", ip_addr);
            return 1; /* Return error */
        }

        VLOG_DBG("ipv4 addr converted = 0x%x", ipv4_dest_addr);
        l3host.l3a_ip_addr = ipv4_dest_addr;
    }

    l3host.l3a_intf = *l3_egress_id;
    l3host.l3a_vrf = vrf_id;
    l3host.l3a_flags = flags;
    rc = opennsl_l3_host_add(hw_unit, &l3host);
    if (rc != OPENNSL_E_NONE) {
        VLOG_ERR ("opennsl_l3_host_add failed: %x", rc);
        return rc;
    }

    return rc;
} /* ops_routing_add_host_entry */

/* Function to delete l3 host entry via ofproto */
int
ops_routing_delete_host_entry(int hw_unit, opennsl_port_t hw_port,
                              opennsl_vrf_t vrf_id, bool is_ipv6_addr,
                              char *ip_addr, opennsl_if_t *l3_egress_id)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_l3_host_t l3host;
    in_addr_t ipv4_dest_addr;
    char ipv6_dest_addr[sizeof(struct in6_addr)];
    int flags = OPENNSL_L3_HOST_LOCAL;


    /* Delete an IP route / Host Entry */
    VLOG_DBG("In ops_routing_delete_host_entry for ip %s", ip_addr);
    opennsl_l3_host_t_init(&l3host);
    if( is_ipv6_addr ) {
        VLOG_DBG("ipv6 addr type");
        flags |= OPENNSL_L3_IP6;

        /* convert string ip into host format */
        rc = inet_pton(AF_INET6, ip_addr, ipv6_dest_addr);
        if ( rc != 1 ) {
            VLOG_ERR("invalid ipv6-%s", ip_addr);
            return 1; /* Return error */
        }

        memcpy(l3host.l3a_ip6_addr, ipv6_dest_addr, sizeof(struct in6_addr));
    } else {
        /* convert string ip into host format */
        ipv4_dest_addr = inet_network(ip_addr);
        if ( ipv4_dest_addr == -1 ) {
            VLOG_ERR("Invalid ip-%s", ip_addr);
            return 1; /* Return error */
        }

        VLOG_DBG("ipv4 addr converted =0x%x", ipv4_dest_addr);
        l3host.l3a_ip_addr = ipv4_dest_addr;
    }

    l3host.l3a_intf = *l3_egress_id;
    l3host.l3a_vrf = vrf_id;
    l3host.l3a_flags = flags;
    rc = opennsl_l3_host_delete(hw_unit, &l3host);
    if (rc != OPENNSL_E_NONE) {
        VLOG_ERR ("opennsl_l3_host_delete failed: %x", rc);
        return rc;
    }

    /* Delete the egress object */
    VLOG_DBG("Deleting egress object for egress-id %d", *l3_egress_id);
    rc = opennsl_l3_egress_destroy(hw_unit, *l3_egress_id);
    if (rc != OPENNSL_E_NONE) {
       VLOG_ERR ("opennsl_egress_destroy failed: %x", rc);
        return rc;
    }

    *l3_egress_id = -1;
    return rc;
} /* ops_routing_delete_host_entry */

/* Ft to read and reset the host hit-bit */
int
ops_routing_get_host_hit(int hw_unit, opennsl_vrf_t vrf_id,
                         bool is_ipv6_addr, char *ip_addr, bool *hit_bit)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_l3_host_t l3host;
    in_addr_t ipv4_dest_addr;
    char ipv6_dest_addr[sizeof(struct in6_addr)];
    int flags = OPENNSL_L3_HOST_LOCAL;

    VLOG_DBG("In ops_routing_get_host_hit for ip %s", ip_addr);
    opennsl_l3_host_t_init(&l3host);
    if( is_ipv6_addr ) {
        VLOG_DBG("ipv6 addr type");
        flags |= OPENNSL_L3_IP6;

        /* convert string ip into host format */
        rc = inet_pton(AF_INET6, ip_addr, ipv6_dest_addr);
        if ( rc != 1 ) {
            VLOG_ERR("invalid ipv6-%s", ip_addr);
            return 1;
        }

        memcpy(l3host.l3a_ip6_addr, ipv6_dest_addr, sizeof(struct in6_addr));
    } else {
        /* convert string ip into host format */
        ipv4_dest_addr = inet_network(ip_addr);
        if ( ipv4_dest_addr == -1 ) {
            VLOG_ERR("Invalid ip-%s", ip_addr);
            return 1;
        }

        VLOG_DBG("ipv4 addr converted =0x%x", ipv4_dest_addr);
        l3host.l3a_ip_addr = ipv4_dest_addr;
    }

    /* Get Host Entry */
    l3host.l3a_vrf = vrf_id;
    l3host.l3a_flags = flags;
    rc = opennsl_l3_host_find(hw_unit, &l3host);
    if (rc != OPENNSL_E_NONE) {
        VLOG_ERR ("opennsl_l3_host_find failed: %x", rc);
        return rc;
    } else {
        *hit_bit = (l3host.l3a_flags & OPENNSL_L3_HIT);
        VLOG_DBG("Got the hit-bit =0x%x", *hit_bit);
        if(*hit_bit) {
            l3host.l3a_flags = flags | OPENNSL_L3_HIT_CLEAR;
            /* Reset the hit-bit */
            rc = opennsl_l3_host_find(hw_unit, &l3host);
            if (rc != OPENNSL_E_NONE) {
                VLOG_ERR ("Reset hit-bit failed: %x", rc);
                return rc;
            }
        }
    }

    return rc;
} /* ops_routing_get_host_hit */

/* Convert from string to ipv4/ipv6 prefix */
static int
ops_string_to_prefix(int family, char *ip_address, void *prefix,
                     unsigned char *prefixlen)
{
    char *p;
    char *tmp_ip_addr;
    int maxlen = (family == AF_INET) ? IPV4_PREFIX_LEN :
                                       IPV6_PREFIX_LEN;
    *prefixlen = maxlen;
    tmp_ip_addr = strdup(ip_address);

    if ((p = strchr(tmp_ip_addr, '/'))) {
        *p++ = '\0';
        *prefixlen = atoi(p);
    }

    if (*prefixlen > maxlen) {
        VLOG_DBG("Bad prefixlen %d > %d", *prefixlen, maxlen);
        free(tmp_ip_addr);
        return EINVAL;
    }

    if (family == AF_INET) {
        /* ipv4 address in host order */
        in_addr_t *addr = (in_addr_t*)prefix;
        *addr = inet_network(tmp_ip_addr);
        if (*addr == -1) {
            VLOG_ERR("Invalid ip address %s", ip_address);
            free(tmp_ip_addr);
            return EINVAL;
        }
    } else {
        /* ipv6 address */
        if (inet_pton(family, tmp_ip_addr, prefix) == 0) {
            VLOG_DBG("%d inet_pton failed with %s", family, strerror(errno));
            free(tmp_ip_addr);
            return EINVAL;
        }
    }

    free(tmp_ip_addr);
    return 0;
} /* ops_string_to_prefix */

/* Find or create and ecmp egress object */
static int
ops_create_or_update_ecmp_object(int hw_unit, struct ops_route *routep,
                                 opennsl_if_t *ecmp_intfp, bool update)
{
    int nh_count = 0;
    struct ops_nexthop *nh;
    opennsl_if_t egress_obj[MAX_NEXTHOPS_PER_ROUTE];
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_l3_egress_ecmp_t ecmp_grp;
    bool install_local_nhid = true;

    if(!routep) {
        return EINVAL;
    }

    HMAP_FOR_EACH(nh, node, &routep->nexthops) {
        if (nh->l3_egress_id == local_nhid) {
         /* install one local_nhid for all unresolved nh */
            if (install_local_nhid) {
                egress_obj[nh_count++] = nh->l3_egress_id;
                install_local_nhid = false;
            }
        } else {
            egress_obj[nh_count++] = nh->l3_egress_id;
        }
        /* break once max ecmp is reached */
        if (nh_count == MAX_NEXTHOPS_PER_ROUTE) {
            break;
        }
    }

    if (update){
        opennsl_l3_egress_ecmp_t_init(&ecmp_grp);
        ecmp_grp.flags = (OPENNSL_L3_REPLACE | OPENNSL_L3_WITH_ID);
        ecmp_grp.ecmp_intf = *ecmp_intfp;
        rc = opennsl_l3_egress_ecmp_create(hw_unit, &ecmp_grp, nh_count,
                                           egress_obj);
        if (OPENNSL_FAILURE(rc)) {
            VLOG_ERR("Failed to update ecmp object for route %s: rc=%s",
                     routep->prefix, opennsl_errmsg(rc));
            return rc;
        }
    } else {
        rc = opennsl_l3_egress_ecmp_find(hw_unit, nh_count, egress_obj,
                                         &ecmp_grp);
        if (OPENNSL_E_NONE == rc) {
            *ecmp_intfp = ecmp_grp.ecmp_intf;
        } else {
            opennsl_l3_egress_ecmp_t_init(&ecmp_grp);
            rc = opennsl_l3_egress_ecmp_create(hw_unit, &ecmp_grp, nh_count,
                                               egress_obj);

            if (OPENNSL_FAILURE(rc)) {
                VLOG_ERR("Failed to create ecmp object for route %s: rc=%s",
                     routep->prefix, opennsl_errmsg(rc));
                return rc;
            }
            *ecmp_intfp = ecmp_grp.ecmp_intf;
        }

    }
    return rc;
} /* ops_create_or_update_ecmp_object */

/* Delete ecmp object */
static int
ops_delete_ecmp_object(int hw_unit, opennsl_if_t ecmp_intf)
{
    opennsl_error_t rc = OPENNSL_E_NONE;
    opennsl_l3_egress_ecmp_t ecmp_grp;

    opennsl_l3_egress_ecmp_t_init(&ecmp_grp);
    ecmp_grp.ecmp_intf = ecmp_intf;

    rc = opennsl_l3_egress_ecmp_destroy(hw_unit, &ecmp_grp);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to delete ecmp egress object %d: %s",
                  ecmp_intf, opennsl_errmsg(rc));
        return rc;
    }
    return rc;
} /* ops_delete_ecmp_object */

/* add or update ECMP or non-ECMP route */
static int
ops_add_route_entry(int hw_unit, opennsl_vrf_t vrf_id,
                    struct ofproto_route *of_routep,
                    opennsl_l3_route_t *routep)
{
    struct ops_route *ops_routep;
    struct ops_nexthop *ops_nh;
    opennsl_if_t l3_intf;
    int rc;
    bool add_route = false;

    /* assert for zero nexthop */
    assert(of_routep && (of_routep->n_nexthops > 0));

    /* look for prefix in LPM table*/
    rc = opennsl_l3_route_get(hw_unit, routep);

    /* Return error other than found / not found */
    if ((rc != OPENNSL_E_NOT_FOUND) &&
        (rc != OPENNSL_E_NONE)) {
        VLOG_ERR("Route lookup error: %s", opennsl_errmsg(rc));
        return rc;
    }

    /* new route */
    if (rc == OPENNSL_E_NOT_FOUND){
        /* add the route in local data structure */
        ops_routep = ops_route_add(vrf_id, of_routep);
        /* create or get ecmp object */
        if (ops_routep->n_nexthops > 1){
            rc = ops_create_or_update_ecmp_object(hw_unit, ops_routep,
                                                 &l3_intf, false);
            if (OPS_FAILURE(rc)) {
                return rc;
            }
            routep->l3a_intf = l3_intf;
            routep->l3a_flags |= OPENNSL_L3_MULTIPATH;
        } else {
            HMAP_FOR_EACH(ops_nh, node, &ops_routep->nexthops) {
                routep->l3a_intf = ops_nh->l3_egress_id;
            }
        }
        add_route = true;
    } else {
        /* update route in local data structure */
        ops_routep = ops_route_lookup(vrf_id, of_routep);
        if (!ops_routep) {
            VLOG_ERR("Failed to find route %s", of_routep->prefix);
            return EINVAL;
        }

        ops_route_update(vrf_id, ops_routep, of_routep, false);

        switch (ops_routep->rstate) {
        case OPS_ROUTE_STATE_NON_ECMP:
            /* if nexthops becomes more than 1 */
            if (ops_routep->n_nexthops > 1) {
                rc = ops_create_or_update_ecmp_object(hw_unit, ops_routep,
                                                     &l3_intf, false);
                if (OPS_FAILURE(rc)) {
                    VLOG_ERR("Failed to create ecmp object for route %s: %s",
                              ops_routep->prefix, opennsl_errmsg(rc));
                    return rc;
                }
                routep->l3a_intf = l3_intf;
                routep->l3a_flags |= (OPENNSL_L3_MULTIPATH |
                                      OPENNSL_L3_REPLACE);
            } else {
                HMAP_FOR_EACH(ops_nh, node, &ops_routep->nexthops) {
                    routep->l3a_intf = ops_nh->l3_egress_id;
                    routep->l3a_flags &= ~OPENNSL_L3_MULTIPATH;
                    routep->l3a_flags |= OPENNSL_L3_REPLACE;
                }
            }
            break;
        case OPS_ROUTE_STATE_ECMP:
            /* update the ecmp table */
            l3_intf = routep->l3a_intf;
            rc = ops_create_or_update_ecmp_object(hw_unit, ops_routep,
                                                 &l3_intf, true);
            if (OPS_FAILURE(rc)) {
                VLOG_ERR("Failed to update ecmp object for route %s: %s",
                         ops_routep->prefix, opennsl_errmsg(rc));
                return rc;
            }
            routep->l3a_flags |= (OPENNSL_L3_MULTIPATH |
                                  OPENNSL_L3_REPLACE);
            break;
        default:
            break;
        }

    }
    ops_routep->rstate = (ops_routep->n_nexthops > 1) ?
                         OPS_ROUTE_STATE_ECMP : OPS_ROUTE_STATE_NON_ECMP;

    rc = opennsl_l3_route_add(hw_unit, routep);
    if (rc != OPENNSL_E_NONE) {
        VLOG_ERR("Failed to %s route %s: %s",
                  add_route ? "add" : "update", of_routep->prefix,
                  opennsl_errmsg(rc));
    } else {
        VLOG_DBG("Success to %s route %s: %s",
                  add_route ? "add" : "update", of_routep->prefix,
                  opennsl_errmsg(rc));
    }
    return rc;
} /* ops_add_route_entry */

/* Delete a route entry */
static int
ops_delete_route_entry(int hw_unit, opennsl_vrf_t vrf_id,
                       struct ofproto_route *of_routep,
                       opennsl_l3_route_t *routep)
{
    struct ops_route *ops_routep;
    opennsl_if_t l3_intf ;
    bool is_delete_ecmp = false;
    int rc;

    assert(of_routep);

    /* look for prefix in LPM table*/
    rc = opennsl_l3_route_get(hw_unit, routep);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Route lookup error: %s", opennsl_errmsg(rc));
        return rc;
    }

    /* route lookup in local data structure */
    ops_routep = ops_route_lookup(vrf_id, of_routep);
    if (!ops_routep) {
        VLOG_ERR("Failed to get route %s", of_routep->prefix);
        return EINVAL;
    }

    ops_route_delete(ops_routep);

    if (routep->l3a_flags & OPENNSL_L3_MULTIPATH) {
        l3_intf = routep->l3a_intf;
        is_delete_ecmp = true;
    }

    rc = opennsl_l3_route_delete(hw_unit, routep);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to delete route %s: %s", of_routep->prefix,
                  opennsl_errmsg(rc));
    } else {
        VLOG_DBG("Success to delete route %s: %s", of_routep->prefix,
                 opennsl_errmsg(rc));
    }

    if (is_delete_ecmp) {
        rc = ops_delete_ecmp_object(hw_unit, l3_intf);
    }
    return rc;
} /* ops_delete_route_entry */

/* Delete nexthop entry in route table */
static int
ops_delete_nh_entry(int hw_unit, opennsl_vrf_t vrf_id,
                    struct ofproto_route *of_routep,
                    opennsl_l3_route_t *routep)
{
    struct ops_route *ops_routep;
    struct ops_nexthop *ops_nh;
    opennsl_if_t l3_intf;
    bool is_delete_ecmp = false;
    int rc;

    /* assert for zero nexthop */
    assert(of_routep && (of_routep->n_nexthops > 0));

    /* look for prefix in LPM table*/
    rc = opennsl_l3_route_get(hw_unit, routep);

    /* Return error other than found / not found */
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Route lookup error: %s", opennsl_errmsg(rc));
        return rc;
    }

    /* route lookup in local data structure */
    ops_routep = ops_route_lookup(vrf_id, of_routep);
    if (!ops_routep) {
        VLOG_ERR("Failed to get route %s", of_routep->prefix);
        return EINVAL;
    }
    ops_route_update(vrf_id, ops_routep, of_routep, true);

    switch (ops_routep->rstate) {
    case OPS_ROUTE_STATE_NON_ECMP:
        HMAP_FOR_EACH(ops_nh, node, &ops_routep->nexthops) {
            routep->l3a_intf = ops_nh->l3_egress_id;
            routep->l3a_flags &= ~OPENNSL_L3_MULTIPATH;
            routep->l3a_flags |= OPENNSL_L3_REPLACE;
        }
        break;
    case OPS_ROUTE_STATE_ECMP:
        /* ecmp route to non-ecmp route*/
        if (ops_routep->n_nexthops < 2) {
            /* delete ecmp table if route has single nexthop */
            if (routep->l3a_flags & OPENNSL_L3_MULTIPATH) {
                /* store intf to delete ecmp object */
                l3_intf = routep->l3a_intf;
                is_delete_ecmp = true;
            }
            /* update with single nexthop */
            HMAP_FOR_EACH(ops_nh, node, &ops_routep->nexthops) {
                routep->l3a_intf = ops_nh->l3_egress_id;
            }
            routep->l3a_flags &= ~OPENNSL_L3_MULTIPATH;
            routep->l3a_flags |= OPENNSL_L3_REPLACE;
        } else {
            /* update the ecmp table */
            l3_intf = routep->l3a_intf;
            rc = ops_create_or_update_ecmp_object(hw_unit, ops_routep,
                                                 &l3_intf, false);
            if (OPS_FAILURE(rc)) {
                VLOG_ERR("Failed to update ecmp object for route %s: %s",
                              ops_routep->prefix, opennsl_errmsg(rc));
                    return rc;
                }
                routep->l3a_flags |= (OPENNSL_L3_MULTIPATH |
                                      OPENNSL_L3_REPLACE);
            }
            break;
        default:
            break;
    }
    ops_routep->rstate = (ops_routep->n_nexthops > 1) ?
                          OPS_ROUTE_STATE_ECMP : OPS_ROUTE_STATE_NON_ECMP;

    rc = opennsl_l3_route_add(hw_unit, routep);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to (delete NH) update route %s: %s",
                  of_routep->prefix, opennsl_errmsg(rc));
        return rc;
    } else {
        VLOG_DBG("Success to (delete NH) update route %s: %s",
                  of_routep->prefix, opennsl_errmsg(rc));
    }

    if (is_delete_ecmp) {
        rc = ops_delete_ecmp_object(hw_unit, l3_intf);
    }
    return rc;
} /* ops_delete_nh_entry */

/* Add, delete route and nexthop */
int
ops_routing_route_entry_action(int hw_unit,
                               opennsl_vrf_t vrf_id,
                               enum ofproto_route_action action,
                               struct ofproto_route *routep)
{
    int rc = 0;
    opennsl_l3_route_t route;
    in_addr_t ipv4_addr;
    struct in6_addr ipv6_addr;
    uint8_t prefix_len;

    VLOG_DBG("%s: vrfid: %d, action: %d", __FUNCTION__, vrf_id, action);

    if (!routep && !routep->n_nexthops) {
        VLOG_ERR("route/nexthop entry null");
        return EINVAL; /* Return error */
    }

    opennsl_l3_route_t_init(&route);

    switch (routep->family) {
    case OFPROTO_ROUTE_IPV4:
        ops_string_to_prefix(AF_INET, routep->prefix, &ipv4_addr, &prefix_len);
        route.l3a_subnet = ipv4_addr;
        route.l3a_ip_mask = opennsl_ip_mask_create(prefix_len);
        break;
    case OFPROTO_ROUTE_IPV6:
        ops_string_to_prefix(AF_INET6, routep->prefix, &ipv6_addr, &prefix_len);
        route.l3a_flags |= OPENNSL_L3_IP6;
        memcpy(route.l3a_ip6_net, &ipv6_addr, sizeof(struct in6_addr));
        opennsl_ip6_mask_create(route.l3a_ip6_mask, prefix_len);
        break;
     default:
        VLOG_ERR ("Unknown protocol %d", routep->family);
        return EINVAL;

    }
    route.l3a_vrf = vrf_id;

    VLOG_DBG("action: %d, vrf: %d, prefix: %s, nexthops: %d",
              action, vrf_id, routep->prefix, routep->n_nexthops);

    switch (action) {
    case OFPROTO_ROUTE_ADD:
        rc = ops_add_route_entry(hw_unit, vrf_id, routep, &route);
        break;
    case OFPROTO_ROUTE_DELETE:
        rc = ops_delete_route_entry(hw_unit, vrf_id, routep, &route);
        break;
    case OFPROTO_ROUTE_DELETE_NH:
        rc = ops_delete_nh_entry(hw_unit, vrf_id, routep, &route);
        break;
    default:
        VLOG_ERR("Unknown route action %d", action);
        rc = EINVAL;
        break;
    }

    return rc;
} /* ops_routing_route_entry_action */

/* OPS_TODO : Remove once these macros are exposed by opennsl */
#define opennslSwitchHashMultipath (135)
#define OPENNSL_HASH_ZERO          0x00000001
int
ops_routing_ecmp_set(int hw_unit, bool enable)
{
    int cur_cfg = 0;
    static int last_cfg;
    opennsl_error_t rc = OPENNSL_E_NONE;

    rc = opennsl_switch_control_get(hw_unit, opennslSwitchHashMultipath,
                                    &cur_cfg);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to get opennslSwitchHashMultipath : unit=%d, rc=%s",
                 hw_unit, opennsl_errmsg(rc));
        return rc;
    }

    /* check if already in the desired state */
    if (((cur_cfg != OPENNSL_HASH_ZERO) && enable) ||
        ((cur_cfg == OPENNSL_HASH_ZERO) && !enable)) {
        return OPENNSL_E_NONE;
    }

    if (enable) { /* Enable ECMP */
        /* write back the config that existed before disabling */
        rc = opennsl_switch_control_set(hw_unit, opennslSwitchHashMultipath,
                                        last_cfg);
        if (OPENNSL_FAILURE(rc)) {
            VLOG_ERR("Failed to set opennslSwitchHashMultipath : unit=%d, rc=%s",
                     hw_unit, opennsl_errmsg(rc));
            return rc;
        }
    } else { /* Disable ECMP */
        /* save the current config before disabling */
        last_cfg = cur_cfg;
        rc = opennsl_switch_control_set(hw_unit, opennslSwitchHashMultipath,
                                        OPENNSL_HASH_ZERO);
        if (OPENNSL_FAILURE(rc)) {
            VLOG_ERR("Failed to clear opennslSwitchHashMultipath : unit=%d, rc=%s",
                     hw_unit, opennsl_errmsg(rc));
            return rc;
        }
    }
    return OPENNSL_E_NONE;
}

int
ops_routing_ecmp_hash_set(int hw_unit, unsigned int hash, bool enable)
{
    int hash_v4 = 0, hash_v6 = 0;
    int cur_hash_ip4 = 0, cur_hash_ip6 = 0;
    opennsl_error_t rc = OPENNSL_E_NONE;

    rc = opennsl_switch_control_get(hw_unit, opennslSwitchHashIP4Field0,
                                    &cur_hash_ip4);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to get opennslSwitchHashIP4Field0 : unit=%d, rc=%s",
                 hw_unit, opennsl_errmsg(rc));
        return rc;
    }
    rc = opennsl_switch_control_get(hw_unit, opennslSwitchHashIP6Field0,
                                    &cur_hash_ip6);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to get opennslSwitchHashIP6Field0 : unit=%d, rc=%s",
                 hw_unit, opennsl_errmsg(rc));
        return rc;
    }

    if (hash & OFPROTO_ECMP_HASH_SRCPORT) {
        hash_v4 |= OPENNSL_HASH_FIELD_SRCL4;
        hash_v6 |= OPENNSL_HASH_FIELD_SRCL4;
    }
    if (hash & OFPROTO_ECMP_HASH_DSTPORT) {
        hash_v4 |= OPENNSL_HASH_FIELD_DSTL4;
        hash_v6 |= OPENNSL_HASH_FIELD_DSTL4;
    }
    if (hash & OFPROTO_ECMP_HASH_SRCIP) {
        hash_v4 |= OPENNSL_HASH_FIELD_IP4SRC_LO | OPENNSL_HASH_FIELD_IP4SRC_HI;
        hash_v6 |= OPENNSL_HASH_FIELD_IP6SRC_LO | OPENNSL_HASH_FIELD_IP6SRC_HI;
    }
    if (hash & OFPROTO_ECMP_HASH_DSTIP) {
        hash_v4 |= OPENNSL_HASH_FIELD_IP4DST_LO | OPENNSL_HASH_FIELD_IP4DST_HI;
        hash_v6 |= OPENNSL_HASH_FIELD_IP6DST_LO | OPENNSL_HASH_FIELD_IP6DST_HI;
    }

    if (enable) {
        cur_hash_ip4 |= hash_v4;
        cur_hash_ip6 |= hash_v6;
    } else {
        cur_hash_ip4 &= ~hash_v4;
        cur_hash_ip6 &= ~hash_v6;
    }

    rc = opennsl_switch_control_set(hw_unit, opennslSwitchHashIP4Field0,
                                    cur_hash_ip4);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchHashIP4Field0 : unit=%d, hash=%x, rc=%s",
                 hw_unit, cur_hash_ip4, opennsl_errmsg(rc));
        return rc;
    }
    rc = opennsl_switch_control_set(hw_unit, opennslSwitchHashIP6Field0,
                                    cur_hash_ip6);
    if (OPENNSL_FAILURE(rc)) {
        VLOG_ERR("Failed to set opennslSwitchHashIP6Field0 : unit=%d, hash=%x, rc=%s",
                 hw_unit, cur_hash_ip6, opennsl_errmsg(rc));
        return rc;
    }

    return OPENNSL_E_NONE;
}

static void
l3_intf_print(struct ds *ds, int unit, int print_hdr,
              opennsl_l3_intf_t *intf)
{
    char if_mac_str[SAL_MACADDR_STR_LEN];

    if (NULL == intf) {
        return;
    }

    if (print_hdr) {
         ds_put_format(ds ,"Unit  Intf  VRF VLAN    Source Mac     MTU TTL\n");
         ds_put_format(ds ,"----------------------------------------------\n");
    }

    snprintf(if_mac_str, SAL_MACADDR_STR_LEN, "%s",
             ether_ntoa((struct ether_addr*)intf->l3a_mac_addr));

    ds_put_format(ds ,"%-5d %-5d %-5d %-4d %-18s %-4d %-4d\n",
                  unit, intf->l3a_intf_id, intf->l3a_vrf, intf->l3a_vid,
                  if_mac_str, intf->l3a_mtu, intf->l3a_ttl);
    return;
} /* l3_intf_print */

void
ops_l3intf_dump(struct ds *ds, int intfid)
{
    int               unit;
    int               rv;
    int               free_l3intf;
    opennsl_l3_info_t l3_hw_status;
    opennsl_l3_intf_t intf;
    int               print_hdr = TRUE;

    for (unit = 0; unit <= MAX_SWITCH_UNIT_ID; unit++) {
        rv = opennsl_l3_info(unit, &l3_hw_status);
        if (OPENNSL_FAILURE(rv)){
            VLOG_ERR("Error in L3 info access: %s\n",
                     opennsl_errmsg(rv));
            return;
        }

        free_l3intf = l3_hw_status.l3info_max_intf -
                      l3_hw_status.l3info_used_intf;

        if (intfid != -1) {
            if ((intfid < 0) || (intfid > l3_hw_status.l3info_max_intf)) {
                VLOG_ERR("Invalid interface index: %d\n", intfid);
                return;
            }
        }
        ds_put_format(ds, "Free L3INTF entries: %d\n", free_l3intf);

        if (intfid != -1) {
            opennsl_l3_intf_t_init(&intf);
            intf.l3a_intf_id = intfid;
            rv = opennsl_l3_intf_get(unit, &intf);
            if (OPENNSL_SUCCESS(rv)) {
                l3_intf_print(ds, unit, TRUE, &intf);
                return ;
            } else {
                VLOG_ERR("Error L3 interface %d: %s\n", intfid,
                         opennsl_errmsg(rv));
                return;
            }
       } else {
            /* Note: last interface id is reserved for Copy To CPU purposes. */
            for (intfid = 0; intfid < l3_hw_status.l3info_max_intf - 1;
                 intfid++) {
                opennsl_l3_intf_t_init(&intf);
                intf.l3a_intf_id = intfid;
                rv = opennsl_l3_intf_get(unit, &intf);
                if (OPENNSL_SUCCESS(rv)) {
                    l3_intf_print(ds, unit, print_hdr, &intf);
                    print_hdr = FALSE;
                } else if (rv == OPENNSL_E_NOT_FOUND) {
                    continue;
                } else if (OPENNSL_FAILURE(rv)) {
                    VLOG_ERR("Error traverse l3 interfaces: %s\n",
                             opennsl_errmsg(rv));
                }
            }
        }
    }
} /* ops_l3intf_dump */

int ops_host_print(int unit, int index, opennsl_l3_host_t *info,
                   void *user_data)
{
    char *hit;
    char *trunk = " ";
    int     port = 0;
    struct ds *pds = (struct ds *)user_data;

    hit = (info->l3a_flags & OPENNSL_L3_HIT) ? "y" : "n";

    if (info->l3a_flags & OPENNSL_L3_TGID) {
        trunk = "t" ;
    }
    port = info->l3a_port_tgid;

    if (info->l3a_flags & OPENNSL_L3_IP6) {
        char ip_str[IPV6_PREFIX_LEN];
        sprintf(ip_str, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
            (((uint16)info->l3a_ip6_addr[0] << 8) | info->l3a_ip6_addr[1]),
            (((uint16)info->l3a_ip6_addr[2] << 8) | info->l3a_ip6_addr[3]),
            (((uint16)info->l3a_ip6_addr[4] << 8) | info->l3a_ip6_addr[5]),
            (((uint16)info->l3a_ip6_addr[6] << 8) | info->l3a_ip6_addr[7]),
            (((uint16)info->l3a_ip6_addr[8] << 8) | info->l3a_ip6_addr[9]),
            (((uint16)info->l3a_ip6_addr[10] << 8) | info->l3a_ip6_addr[11]),
            (((uint16)info->l3a_ip6_addr[12] << 8) | info->l3a_ip6_addr[13]),
            (((uint16)info->l3a_ip6_addr[14] << 8) | info->l3a_ip6_addr[15]));

        ds_put_format(pds, "%-6d %-4d %-42s %d %4d%s %5s\n", index,
                info->l3a_vrf, ip_str, info->l3a_intf,
                port, trunk, hit);
    } else {
        char ip_str[IPV4_PREFIX_LEN];
        sprintf(ip_str, "%d.%d.%d.%d",
            (info->l3a_ip_addr >> 24) & 0xff, (info->l3a_ip_addr >> 16) & 0xff,
            (info->l3a_ip_addr >> 8) & 0xff, info->l3a_ip_addr & 0xff);

        ds_put_format(pds,"%-6d %-4d %-16s %2d %4d%s %6s\n",
                index, info->l3a_vrf, ip_str, info->l3a_intf,
                port, trunk, hit);
    }

    return OPENNSL_E_NONE;
} /*ops_host_print*/

void
ops_l3host_dump(struct ds *ds, int ipv6_enabled)
{
    int               unit = 0;
    int               rv;
    int               last_entry;
    int               first_entry;
    opennsl_l3_info_t l3_hw_status;

    rv = opennsl_l3_info(unit, &l3_hw_status);
    if (OPENNSL_FAILURE(rv)){
        VLOG_ERR("Error in L3 info access: %s\n",opennsl_errmsg(rv));
        return;
    }

    last_entry = l3_hw_status.l3info_max_host;
    first_entry = 0;

    if (ipv6_enabled == TRUE) {
        ds_put_format(ds ,"Entry VRF                 IP address                 "
                          "INTF PORT    HIT \n");
        ds_put_format(ds ,"------------------------------------------------"
                           "---------------------\n");
        opennsl_l3_host_traverse(unit, OPENNSL_L3_IP6, first_entry, last_entry,
                                 &ops_host_print, ds);
    } else {
        ds_put_format(ds ,"Entry VRF     IP address     INTF PORT    HIT \n");
        ds_put_format(ds ,"-----------------------------------------------\n");
        opennsl_l3_host_traverse(unit, 0, first_entry, last_entry,
                                 &ops_host_print, ds);
    }

} /* ops_l3host_dump */

int ops_route_print(int unit, int index, opennsl_l3_route_t *info,
                    void *user_data)
{
    char *hit;
    char *ecmp_str;
    struct ds *pds = (struct ds *)user_data;

    hit = (info->l3a_flags & OPENNSL_L3_HIT) ? "Y" : "N";
    ecmp_str = (info->l3a_flags & OPENNSL_L3_MULTIPATH) ? "(ECMP)" : "";

    if (info->l3a_flags & OPENNSL_L3_IP6) {
        char subnet_str[IPV6_PREFIX_LEN];
        char subnet_mask[IPV6_PREFIX_LEN];
        sprintf(subnet_str, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
            (((uint16)info->l3a_ip6_net[0] << 8) | info->l3a_ip6_net[1]),
            (((uint16)info->l3a_ip6_net[2] << 8) | info->l3a_ip6_net[3]),
            (((uint16)info->l3a_ip6_net[4] << 8) | info->l3a_ip6_net[5]),
            (((uint16)info->l3a_ip6_net[6] << 8) | info->l3a_ip6_net[7]),
            (((uint16)info->l3a_ip6_net[8] << 8) | info->l3a_ip6_net[9]),
            (((uint16)info->l3a_ip6_net[10] << 8) | info->l3a_ip6_net[11]),
            (((uint16)info->l3a_ip6_net[12] << 8) | info->l3a_ip6_net[13]),
            (((uint16)info->l3a_ip6_net[14] << 8) | info->l3a_ip6_net[15]));
        sprintf(subnet_mask, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
            (((uint16)info->l3a_ip6_mask[0] << 8) | info->l3a_ip6_mask[1]),
            (((uint16)info->l3a_ip6_mask[2] << 8) | info->l3a_ip6_mask[3]),
            (((uint16)info->l3a_ip6_mask[4] << 8) | info->l3a_ip6_mask[5]),
            (((uint16)info->l3a_ip6_mask[6] << 8) | info->l3a_ip6_mask[7]),
            (((uint16)info->l3a_ip6_mask[8] << 8) | info->l3a_ip6_mask[9]),
            (((uint16)info->l3a_ip6_mask[10] << 8) | info->l3a_ip6_mask[11]),
            (((uint16)info->l3a_ip6_mask[12] << 8) | info->l3a_ip6_mask[13]),
            (((uint16)info->l3a_ip6_mask[14] << 8) | info->l3a_ip6_mask[15]));

        ds_put_format(pds, "%-6d %-4d %-42s %-42s %2d %4s %s\n", index,
                      info->l3a_vrf, subnet_str, subnet_mask, info->l3a_intf,
                      hit, ecmp_str);
    } else {
        char subnet_str[IPV4_PREFIX_LEN];
        char subnet_mask[IPV4_PREFIX_LEN];
        sprintf(subnet_str, "%d.%d.%d.%d",
            (info->l3a_subnet >> 24) & 0xff, (info->l3a_subnet >> 16) & 0xff,
            (info->l3a_subnet >> 8) & 0xff, info->l3a_subnet & 0xff);
        sprintf(subnet_mask, "%d.%d.%d.%d",
            (info->l3a_ip_mask >> 24) & 0xff, (info->l3a_ip_mask >> 16) & 0xff,
            (info->l3a_ip_mask >> 8) & 0xff, info->l3a_ip_mask & 0xff);

        ds_put_format(pds,"%-6d %-4d %-16s %-16s %2d %5s %s\n", index,
                      info->l3a_vrf, subnet_str, subnet_mask, info->l3a_intf,
                      hit, ecmp_str);
    }
    return OPENNSL_E_NONE;
} /*ops_route_print*/

void
ops_l3route_dump(struct ds *ds, int ipv6_enabled)
{
    int               unit = 0;
    int               rv;
    int               last_entry;
    int               first_entry;
    opennsl_l3_info_t l3_hw_status;

    rv = opennsl_l3_info(unit, &l3_hw_status);
    if (OPENNSL_FAILURE(rv)){
        VLOG_ERR("Error in L3 info access: %s\n",opennsl_errmsg(rv));
        return;
    }

    last_entry = l3_hw_status.l3info_max_route;
    first_entry = 0;
    /*
     * OPS_TODO: We need the l3info_used_route to display the number of
     * entries used
     */

    if (ipv6_enabled == TRUE) {
        int ipv6_hash = 0;
        rv = opennsl_switch_control_get(unit, opennslSwitchHashIP6Field0,
                                        &ipv6_hash);
        if (OPENNSL_FAILURE(rv)){
            VLOG_ERR("Error in get opennslSwitchHashIP6Field0: %s\n",
                      opennsl_errmsg(rv));
            return;
        }
        ds_put_format(ds ,"ECMP IPv6 hash\n");
        ds_put_format(ds ,"Src Addr    Dst Addr    Src Port    Dst Port\n");
        ds_put_format(ds ,"--------------------------------------------\n");
        ds_put_format(ds ,"%4s %11s %11s %11s\n\n",
                          ((ipv6_hash & OPENNSL_HASH_FIELD_IP6SRC_LO) ||
                           (ipv6_hash & OPENNSL_HASH_FIELD_IP6SRC_HI)) ? "Y" : "N",
                          ((ipv6_hash & OPENNSL_HASH_FIELD_IP6DST_LO) ||
                           (ipv6_hash & OPENNSL_HASH_FIELD_IP6DST_HI)) ? "Y" : "N",
                          (ipv6_hash & OPENNSL_HASH_FIELD_SRCL4) ? "Y" : "N",
                          (ipv6_hash & OPENNSL_HASH_FIELD_DSTL4) ? "Y" : "N");
        ds_put_format(ds ,"Entry VRF                Subnet                      "
                      "                 Mask                         I/F     HIT \n");
        ds_put_format(ds ,"-----------------------------------------------------"
                      "----------------------------------------------------------\n");
        opennsl_l3_route_traverse(unit, OPENNSL_L3_IP6, first_entry, last_entry,
                                 &ops_route_print, ds);
    } else {
        int ipv4_hash = 0;
        rv = opennsl_switch_control_get(unit, opennslSwitchHashIP4Field0,
                                        &ipv4_hash);
        if (OPENNSL_FAILURE(rv)){
            VLOG_ERR("Error in get opennslSwitchHashIP4Field0: %s\n",
                      opennsl_errmsg(rv));
            return;
        }
        ds_put_format(ds ,"ECMP IPv4 hash\n");
        ds_put_format(ds ,"Src Addr    Dst Addr    Src Port    Dst Port\n");
        ds_put_format(ds ,"--------------------------------------------\n");
        ds_put_format(ds ,"%4s %11s %11s %11s\n\n",
                          ((ipv4_hash & OPENNSL_HASH_FIELD_IP4SRC_LO) ||
                           (ipv4_hash & OPENNSL_HASH_FIELD_IP4SRC_HI)) ? "Y" : "N",
                          ((ipv4_hash & OPENNSL_HASH_FIELD_IP4DST_LO) ||
                           (ipv4_hash & OPENNSL_HASH_FIELD_IP4DST_HI)) ? "Y" : "N",
                          (ipv4_hash & OPENNSL_HASH_FIELD_SRCL4) ? "Y" : "N",
                          (ipv4_hash & OPENNSL_HASH_FIELD_DSTL4) ? "Y" : "N");
        ds_put_format(ds, "Entry VRF    Subnet             Mask            I/F     HIT \n");
        ds_put_format(ds ,"------------------------------------------------------------\n");

        opennsl_l3_route_traverse(unit, 0, first_entry, last_entry,
                                 &ops_route_print, ds);
    }

} /* ops_l3route_dump */