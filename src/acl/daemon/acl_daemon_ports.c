/*
 * (c) Copyright 2016 Hewlett Packard Enterprise Development LP
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
/************************************************************************//**
 * @ingroup acl_daemon_ports
 *
 * @file
 * Source for port related processing required by ACL feature in classifier
 * daemon.
 *
 ***************************************************************************/
#include <openvswitch/vlog.h>
#include <assert.h>
#include <vswitch-idl.h>
#include <acl_daemon.h>
#include "acl_db_util.h"
#include "ops-cls-asic-plugin.h"
#include "ops_cls_status_msgs.h"

VLOG_DEFINE_THIS_MODULE(acl_daemon_ports);

/* This function determines if hw_ready_state can be set for the interface
 * associated with the port_row.
 */
static bool
acl_ports_is_hw_ready(const struct ovsrec_port *port_row)
{
    const char *status_str = NULL;
    int i;

    ovs_assert(port_row);

    /* set hw_ready_state to true for following conditions,
     * - If ACL is NOT configured for this port
     * - If ACL is configured and applied status is success
     */
    /* @todo: need to handle multiple acls to the port
     */
    for (i = ACL_CFG_MIN_PORT_TYPES; i <= ACL_CFG_MAX_PORT_TYPES; i++) {
        const struct ovsrec_acl *acl_row =
            acl_db_util_get_cfg(&acl_db_accessor[i], port_row);
        if(!acl_row) {
            VLOG_DBG("port %s: ACL not configured \n", port_row->name);
            /* do not block hw_ready on the interface due to this ACL */
        } else {
            /* ACL is configured on this port so verify if
             * ACL is applied successfully in hw or not
             */
            const struct smap acl_status =
                acl_db_util_get_cfg_status(&acl_db_accessor[i], port_row);

            status_str = smap_get(&acl_status, OPS_CLS_STATUS_CODE_STR);

            VLOG_DBG("port %s: ACL %s configured, apply status %s \n",
                      port_row->name, acl_row->name,
                      status_str);

            if (status_str == NULL) {
                return false;
            }

            if(strtoul(status_str, NULL, 10) != OPS_CLS_STATUS_SUCCESS) {
                /* block hw_ready on this interface */
                return false;
            }
        } /* end if !applied */
    }

    return true;
}

/**
 * Processes port row to determine if hw_ready_state for each
 * interface needs to be set to true or false
 */
static int
acl_port_reconfigure_all(const struct ovsrec_port *port_row)
{
    int rc = 0;
    unsigned int intf_idx;
    const char *hw_status = NULL;
    bool hw_ready_state = false;

    ovs_assert(port_row);

    VLOG_DBG("%s: port name: %s\n",__FUNCTION__,port_row->name);

    hw_ready_state = acl_ports_is_hw_ready(port_row);

    for(intf_idx = 0; intf_idx < port_row->n_interfaces; intf_idx++) {
        if (port_row->interfaces[intf_idx] == NULL) {
            VLOG_WARN("Port %s: linked to NULL interface.\n",port_row->name);
            continue;
        }

        hw_status =
             smap_get(
               (const struct smap *)&port_row->interfaces[intf_idx]->hw_status,
               OPS_INTF_HW_READY_KEY_STR);

        if (hw_ready_state) {

            if((hw_status == NULL) ||
               (strncmp(hw_status,
                        OPS_INTF_HW_READY_VALUE_STR_FALSE,
                        strlen(OPS_INTF_HW_READY_VALUE_STR_FALSE)) == 0)) {

                /* If hw_ready was blocked due to acls, set the hw_status
                   to true and delete hw_blocked_reason key */
                hw_status =
                 smap_get(
                  (const struct smap *)&port_row->interfaces[intf_idx]->hw_status,
                  OPS_INTF_HW_READY_BLOCKED_REASON_STR);
                if((hw_status != NULL) &&
                   (strncmp(
                     hw_status,
                     OPS_INTF_HW_READY_BLOCKED_REASON_VALUE_STR_ACLS,
                     strlen(
                         OPS_INTF_HW_READY_BLOCKED_REASON_VALUE_STR_ACLS)) == 0)) {

                    VLOG_DBG("port %s: setting hw_ready_state to true on "
                             "interface %s\n", port_row->name,
                             port_row->interfaces[intf_idx]->name);

                    /* set interface hw_ready_state to true in db */
                    ovsrec_interface_update_hw_status_setkey(
                                                 port_row->interfaces[intf_idx],
                                                 OPS_INTF_HW_READY_KEY_STR,
                                                 OPS_INTF_HW_READY_VALUE_STR_TRUE);

                    ovsrec_interface_update_hw_status_delkey(
                                             port_row->interfaces[intf_idx],
                                             OPS_INTF_HW_READY_BLOCKED_REASON_STR);

                    /* increment rc to indicate db update */
                    rc++;
                }
            }
        }
        else
        {
            if((hw_status == NULL) ||
               (strncmp(hw_status,
                        OPS_INTF_HW_READY_VALUE_STR_TRUE,
                        strlen(OPS_INTF_HW_READY_VALUE_STR_TRUE)) == 0)) {

                /* set interface hw_ready_state to false in db */
                ovsrec_interface_update_hw_status_setkey(
                                            port_row->interfaces[intf_idx],
                                            OPS_INTF_HW_READY_KEY_STR,
                                            OPS_INTF_HW_READY_VALUE_STR_FALSE);

                /* set interface hw_ready_blocked_reason in db */
                ovsrec_interface_update_hw_status_setkey(
                                 port_row->interfaces[intf_idx],
                                 OPS_INTF_HW_READY_BLOCKED_REASON_STR,
                                 OPS_INTF_HW_READY_BLOCKED_REASON_VALUE_STR_ACLS);

                /* increment rc to indicate db update */
                rc++;
            }
        }
    } /* end for loop */

    return rc;
}

/**
 * Process port table changes to determine if interface hw_ready_state
 * needs to be set to true or false for each port row entry
 */
int
acl_ports_reconfigure(struct ovsdb_idl *idl, unsigned int idl_seqno)
{
    int rc = 0;
    const struct ovsrec_port *port_row = NULL;

    VLOG_DBG("%s: idl_seqno %d\n",__FUNCTION__,idl_seqno);
    ovs_assert(idl);

    OVSREC_PORT_FOR_EACH (port_row, idl) {
        if(port_row->n_interfaces == 0) {
            VLOG_DBG("Port %s: No interfaces assigned yet.\n",port_row->name);
            continue;
        }

        rc += acl_port_reconfigure_all(port_row);
    } /* for each port ROW */

    VLOG_DBG("%s: number of updates back to db: %d",__FUNCTION__,rc);

    return rc;
} /* acl_ports_reconfigure */


/** @} end of group acl_daemon_ports */
