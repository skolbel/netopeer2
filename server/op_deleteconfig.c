/**
 * @file op_deleteconfig.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief NETCONF <delete-config> operation implementation
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <string.h>

#include <libyang/libyang.h>
#include <nc_server.h>
#include <sysrepo.h>

#include "common.h"
#include "operations.h"

struct nc_server_reply *
op_deleteconfig(struct lyd_node *rpc, struct nc_session *ncs)
{
    struct np2_sessions *sessions;
    sr_datastore_t target = 0;
    const char *dsname;
    uint32_t index;
    const struct lys_module *mod;
    struct lys_node *iter;
    char path[1024];
    struct ly_set *nodeset;
    struct nc_server_reply *ereply = NULL;
    struct nc_server_error *e;
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
    char *target_url = 0;
    const char* urlval;
#endif

    /* get sysrepo connections for this session */
    sessions = (struct np2_sessions *)nc_session_get_data(ncs);

    if (np2srv_sr_check_exec_permission(sessions->srs, "/ietf-netconf:delete-config", &ereply)) {
        goto finish;
    }

    /* get know which datastore is being affected */
    nodeset = lyd_find_path(rpc, "/ietf-netconf:delete-config/target/*");
    dsname = nodeset->set.d[0]->schema->name;

    if (!strcmp(dsname, "startup")) {
        target = SR_DS_STARTUP;
    } else if (!strcmp(dsname, "url")) {
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
        urlval = ((struct lyd_node_leaf_list*)nodeset->set.d[0])->value_str;
        if (urlval) {
            target_url = (char *)malloc(strlen(urlval) + 1);
            strcpy(target_url, urlval);
            urlval = 0;
        } else {
            ly_set_free(nodeset);
            e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
            nc_err_set_msg(e, "Missing target url", "en");
            ereply = nc_server_reply_err(e);
            goto finish;
        }
#else
        ly_set_free(nodeset);
        e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        nc_err_set_msg(e, "<url> source not supported", "en");
        ereply = nc_server_reply_err(e);
        goto finish;
#endif
    }

    ly_set_free(nodeset);

#ifdef NP2SRV_ENABLED_URL_CAPABILITY
    if (target_url) {
        // Perform delete on the URL

        struct lyd_node *urlcfg;
        // Import URL
        if (op_url_import(target_url, LYD_OPT_CONFIG | LYD_OPT_STRICT, &urlcfg, &ereply)) {
            e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
            nc_err_set_msg(e, "File at url does not appear to contain a valid config", "en");
            nc_server_reply_add_err(ereply, e);
            goto finish;
        }

        lyd_free_withsiblings(urlcfg);
        if (op_url_init(target_url, &ereply)) {
            goto finish;
        }

        ereply = nc_server_reply_ok();
        goto finish;
    }
#endif

    if (sessions->ds != target) {
        /* update sysrepo session */
        if (np2srv_sr_session_switch_ds(sessions->srs, target, &ereply)) {
            goto finish;
        }
        sessions->ds = target;
    }

    /* update data from sysrepo */
    if (np2srv_sr_session_refresh(sessions->srs, &ereply)) {
        goto finish;
    }

    /* perform operation
     * - iterate over all schemas and remove all top-level data nodes.
     * sysrepo does not accept '/\asterisk' since it splits data */
    index = 0;
    while ((mod = ly_ctx_get_module_iter(np2srv.ly_ctx, &index))) {
        LY_TREE_FOR(mod->data, iter) {
            if (!(iter->nodetype & (LYS_CONTAINER | LYS_LIST | LYS_LEAFLIST | LYS_LEAF | LYS_ANYXML)) ||
                    (iter->flags & LYS_CONFIG_R)) {
                /* skip bothering sysrepo with schemas with no configuration data */
                continue;
            }

            snprintf(path, 1024, "/%s:*", mod->name);
            if (np2srv_sr_delete_item(sessions->srs, path, 0, &ereply)) {
                np2srv_sr_discard_changes(sessions->srs, NULL);
                goto finish;
            }

            /* sysrepo was asked for remove all configuration data
             * from this schema so we can continue with another schema */
            break;
        }
    }

    /* commit the result */
    if (np2srv_sr_commit(sessions->srs, &ereply)) {
        np2srv_sr_discard_changes(sessions->srs, NULL);
        goto finish;
    }

    ereply = nc_server_reply_ok();

finish:
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
    if (target_url) {
        free(target_url);
    }
#endif
    return ereply;
}
