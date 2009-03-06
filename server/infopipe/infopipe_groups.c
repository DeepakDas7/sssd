/*
   SSSD

   InfoPipe

   Copyright (C) Stephen Gallagher <sgallagh@redhat.com>	2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <dbus/dbus.h>
#include <ldb.h>
#include <time.h>
#include "util/util.h"
#include "util/btreemap.h"
#include "confdb/confdb.h"
#include "infopipe/infopipe.h"
#include "infopipe/infopipe_private.h"
#include "infopipe/sysbus.h"
#include "db/sysdb.h"

struct infp_creategroup_ctx {
    struct infp_req_ctx *infp_req;
    char **groupnames;
    uint32_t name_count;
    uint32_t index;
    struct sysdb_req *sysdb_req;
};

static void infp_do_group_create(struct sysdb_req *req, void *pvt);
static void infp_do_group_create_callback(void *pvt, int status,
                                          struct ldb_result *res)
{
    char *error_msg = NULL;
    DBusMessage *reply = NULL;
    struct infp_creategroup_ctx *grcreate_req =
        talloc_get_type(pvt, struct infp_creategroup_ctx);

    if (status != EOK) {
        sysdb_transaction_done(grcreate_req->sysdb_req, status);

        if (status == EEXIST) {
            error_msg =
                talloc_asprintf(grcreate_req,
                                "Group [%s] already exists on domain [%s]",
                                grcreate_req->groupnames[grcreate_req->index],
                                grcreate_req->infp_req->domain->name);
            reply = dbus_message_new_error(grcreate_req->infp_req->req_message,
                                           DBUS_ERROR_FILE_EXISTS,
                                           error_msg);
        }
        if (reply)
            sbus_conn_send_reply(grcreate_req->infp_req->sconn, reply);
        talloc_free(grcreate_req);
        return;
    }

    /* Status is okay, add the next group */
    grcreate_req->index++;
    if (grcreate_req->index < grcreate_req->name_count) {
        infp_do_group_create(grcreate_req->sysdb_req, grcreate_req);
        return;
    }

    /* We have no more usernames to add, so commit the transaction */
    sysdb_transaction_done(grcreate_req->sysdb_req, status);
    reply =
        dbus_message_new_method_return(grcreate_req->infp_req->req_message);
    if (reply) sbus_conn_send_reply(grcreate_req->infp_req->sconn, reply);
    talloc_free(grcreate_req);
    return;
}

static void infp_do_group_create(struct sysdb_req *req, void *pvt)
{
    int ret;
    struct infp_creategroup_ctx *grcreate_req =
        talloc_get_type(pvt, struct infp_creategroup_ctx);

    grcreate_req->sysdb_req = req;

    ret = sysdb_add_group(grcreate_req->sysdb_req,
                          grcreate_req->infp_req->domain,
                          grcreate_req->groupnames[grcreate_req->index], 0,
                          infp_do_group_create_callback, grcreate_req);
    if (ret != EOK) {
        DEBUG(0, ("Could not invoke sysdb_add_group\n"));
        sysdb_transaction_done(grcreate_req->sysdb_req, ret);
        talloc_free(grcreate_req);
        return;
    }
}

int infp_groups_create(DBusMessage *message, struct sbus_conn_ctx *sconn)
{
    DBusMessage *reply;
    DBusError error;
    struct infp_creategroup_ctx *grcreate_req;
    char *einval_msg;
    int ret, i;

    /* Arguments */
    char **arg_grnames = NULL;
    int arg_grnames_count;
    const char *arg_domain;

    grcreate_req = talloc_zero(NULL, struct infp_creategroup_ctx);
    if(grcreate_req == NULL) {
        ret = ENOMEM;
        goto error;
    }

    /* Create an infp_req_ctx */
    grcreate_req->infp_req = infp_req_init(grcreate_req, message, sconn);
    if(grcreate_req->infp_req == NULL) {
        ret = EIO;
        goto error;
    }

    dbus_error_init(&error);
    if (!dbus_message_get_args(message, &error,
                               DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
                                 &arg_grnames, &arg_grnames_count,
                               DBUS_TYPE_STRING, &arg_domain,
                               DBUS_TYPE_INVALID)) {
        DEBUG(0, ("Parsing arguments to %s failed: %s:%s\n",
                  INFP_GROUPS_CREATE, error.name, error.message));
        einval_msg = talloc_strdup(grcreate_req, error.message);
        dbus_error_free(&error);
        goto einval;
    }

    /* FIXME: Allow creating groups on domains other than LOCAL */
    if (strcasecmp(arg_domain, "LOCAL") != 0) {
        goto denied;
    }

    grcreate_req->infp_req->domain =
        btreemap_get_value(grcreate_req->infp_req->infp->domain_map,
                           (const void *)arg_domain);
    if(grcreate_req->infp_req->domain == NULL) {
        einval_msg = talloc_strdup(grcreate_req, "Invalid domain.");
        goto einval;
    }

    /* Check permissions */
    if (!infp_get_permissions(grcreate_req->infp_req->caller,
                              grcreate_req->infp_req->domain,
                              INFP_OBJ_TYPE_GROUP,
                              NULL,
                              INFP_ACTION_TYPE_CREATE,
                              INFP_ATTR_TYPE_INVALID)) goto denied;

    grcreate_req->groupnames = talloc_array(grcreate_req,
                                            char *,
                                            arg_grnames_count);
    if (grcreate_req->groupnames == NULL) {
        ret = ENOMEM;
        goto error;
    }

    grcreate_req->name_count = arg_grnames_count;
    for (i = 0; i < arg_grnames_count; i++) {
        grcreate_req->groupnames[i] = talloc_strdup(grcreate_req->groupnames,
                                                    arg_grnames[i]);
        if (grcreate_req->groupnames[i] == NULL) {
            ret = ENOMEM;
            goto error;
        }
    }
    dbus_free_string_array(arg_grnames);
    arg_grnames = NULL;

    grcreate_req->index = 0;
    ret = sysdb_transaction(grcreate_req,
                            grcreate_req->infp_req->infp->sysdb,
                            infp_do_group_create,
                            grcreate_req);

    if (ret != EOK) goto error;

    return EOK;

denied:
    reply = dbus_message_new_error(message, DBUS_ERROR_ACCESS_DENIED, NULL);
    if(reply == NULL) {
        ret = ENOMEM;
        goto error;
    }
    /* send reply */
    sbus_conn_send_reply(sconn, reply);
    dbus_message_unref(reply);

    talloc_free(grcreate_req);
    return EOK;

einval:
    reply = dbus_message_new_error(message,
                                   DBUS_ERROR_INVALID_ARGS,
                                   einval_msg);
    if (reply == NULL) {
        ret = ENOMEM;
        goto error;
    }
    sbus_conn_send_reply(sconn, reply);
    dbus_message_unref(reply);
    if (arg_grnames) dbus_free_string_array(arg_grnames);
    talloc_free(grcreate_req);
    return EOK;

error:
    if (arg_grnames) dbus_free_string_array(arg_grnames);
    talloc_free(grcreate_req);
    return ret;
}

int infp_groups_delete(DBusMessage *message, struct sbus_conn_ctx *sconn)
{
    DBusMessage *reply;

    reply = dbus_message_new_error(message, DBUS_ERROR_NOT_SUPPORTED, "Not yet implemented");

    /* send reply */
    sbus_conn_send_reply(sconn, reply);

    dbus_message_unref(reply);
    return EOK;
}

int infp_groups_add_members(DBusMessage *message, struct sbus_conn_ctx *sconn)
{
    DBusMessage *reply;

    reply = dbus_message_new_error(message, DBUS_ERROR_NOT_SUPPORTED, "Not yet implemented");

    /* send reply */
    sbus_conn_send_reply(sconn, reply);

    dbus_message_unref(reply);
    return EOK;
}

int infp_groups_remove_members(DBusMessage *message, struct sbus_conn_ctx *sconn)
{
    DBusMessage *reply;

    reply = dbus_message_new_error(message, DBUS_ERROR_NOT_SUPPORTED, "Not yet implemented");

    /* send reply */
    sbus_conn_send_reply(sconn, reply);

    dbus_message_unref(reply);
    return EOK;
}

int infp_groups_set_gid(DBusMessage *message, struct sbus_conn_ctx *sconn)
{
    DBusMessage *reply;

    reply = dbus_message_new_error(message, DBUS_ERROR_NOT_SUPPORTED, "Not yet implemented");

    /* send reply */
    sbus_conn_send_reply(sconn, reply);

    dbus_message_unref(reply);
    return EOK;
}
