#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/def.h>
#include <pulse/rtclock.h>
#include <pulse/timeval.h>

#include <pulsecore/core-util.h>
#include <pulsecore/sink.h>
#include <pulsecore/namereg.h>

#include "sink-ext.h"
#include "index-hash.h"
#include "classify.h"
#include "context.h"
#include "policy-group.h"
#include "dbusif.h"

/* hooks */
static pa_hook_result_t sink_put(void *, void *, void *);
static pa_hook_result_t sink_unlink(void *, void *, void *);

static void handle_new_sink(struct userdata *, struct pa_sink *);
static void handle_removed_sink(struct userdata *, struct pa_sink *);

static pa_time_event *delayed_port_change_event = NULL;

struct pa_null_sink *pa_sink_ext_init_null_sink(const char *name)
{
    struct pa_null_sink *null_sink = pa_xnew0(struct pa_null_sink, 1);

    /* sink.null is temporary to de-couple PA releases from ours */
    null_sink->name = pa_xstrdup(name ? name : /* "null" */ "sink.null");
    null_sink->sink = NULL;

    return null_sink;
}

void pa_sink_ext_null_sink_free(struct pa_null_sink *null_sink)
{
    if (null_sink != NULL) {
        pa_xfree(null_sink->name);

        pa_xfree(null_sink);
    }
}

struct pa_sink_evsubscr *pa_sink_ext_subscription(struct userdata *u)
{
    pa_core                 *core;
    pa_hook                 *hooks;
    struct pa_sink_evsubscr *subscr;
    pa_hook_slot            *put;
    pa_hook_slot            *unlink;
    
    pa_assert(u);
    pa_assert_se((core = u->core));

    hooks  = core->hooks;
    
    put    = pa_hook_connect(hooks + PA_CORE_HOOK_SINK_PUT,
                             PA_HOOK_LATE, sink_put, (void *)u);
    unlink = pa_hook_connect(hooks + PA_CORE_HOOK_SINK_UNLINK,
                             PA_HOOK_LATE, sink_unlink, (void *)u);
    

    subscr = pa_xnew0(struct pa_sink_evsubscr, 1);
    
    subscr->put    = put;
    subscr->unlink = unlink;

    return subscr;
}

void  pa_sink_ext_subscription_free(struct pa_sink_evsubscr *subscr)
{
    if (subscr != NULL) {
        pa_hook_slot_free(subscr->put);
        pa_hook_slot_free(subscr->unlink);

        pa_xfree(subscr);
    }
}

void pa_sink_ext_discover(struct userdata *u)
{
    void            *state = NULL;
    pa_idxset       *idxset;
    struct pa_sink  *sink;

    pa_assert(u);
    pa_assert(u->core);
    pa_assert_se((idxset = u->core->sinks));

    while ((sink = pa_idxset_iterate(idxset, &state, NULL)) != NULL)
        handle_new_sink(u, sink);
}


struct pa_sink_ext *pa_sink_ext_lookup(struct userdata *u,struct pa_sink *sink)
{
    struct pa_sink_ext *ext;

    pa_assert(u);
    pa_assert(sink);

    ext = pa_index_hash_lookup(u->hsnk, sink->index);

    return ext;
}


const char *pa_sink_ext_get_name(struct pa_sink *sink)
{
    return sink->name ? sink->name : "<unknown>";
}

struct delayed_port_change {
    char *sink_name;
    char *port_name;
    bool refresh;
};

static struct delayed_port_change change_list[8];
static int change_list_size = 0;

static int set_port(pa_sink *sink, const char *port, bool refresh);

static void delay_cb(pa_mainloop_api *m, pa_time_event *e, const struct timeval *t, void *userdata) {
    struct userdata *u = userdata;
    pa_sink *sink;
    int i;
    int end;

    pa_assert(u);
    pa_assert(delayed_port_change_event == e);

    if (delayed_port_change_event) {
        u->core->mainloop->time_free(delayed_port_change_event);
        delayed_port_change_event = NULL;
    }

    pa_log_info("start delayed port change (%d port changes).", change_list_size);

    end = change_list_size;
    change_list_size = 0;
    for (i = 0; i < end; i++) {
        struct delayed_port_change *c = &change_list[i];

        if ((sink = pa_namereg_get(u->core, c->sink_name, PA_NAMEREG_SINK)))
            set_port(sink, c->port_name, c->refresh);

        pa_xfree(c->sink_name);
        pa_xfree(c->port_name);
    }
}

static void set_port_start(struct userdata *u) {
    if (delayed_port_change_event) {
        u->core->mainloop->time_free(delayed_port_change_event);
        delayed_port_change_event = NULL;
    }

    if (change_list_size > 0) {
        int i;
        for (i = 0; i < change_list_size; i++) {
            pa_xfree(change_list[i].sink_name);
            pa_xfree(change_list[i].port_name);
        }
        change_list_size = 0;
    }
}

static int set_port(pa_sink *sink, const char *port, bool refresh) {
    int ret = 0;

    if (refresh) {
        if (sink->set_port) {
            pa_log_debug("refresh sink '%s' port to '%s'",
                         sink->name, port);
            sink->set_port(sink, sink->active_port);
        }
    } else {
        if (pa_sink_set_port(sink, port, false) < 0) {
            ret = -1;
            pa_log("failed to set sink '%s' port to '%s'",
                   sink->name, port);
        }
        else {
            pa_log_debug("changed sink '%s' port to '%s'",
                         sink->name, port);
        }
    }

    return ret;
}

static int set_port_add(struct userdata *u, pa_sink *sink, const char *port, bool delay, bool refresh) {
    int ret = 0;

    pa_assert(u);
    pa_assert(sink);
    pa_assert(port);

    if (delay) {
        pa_assert(change_list_size < 8);
        change_list[change_list_size].sink_name = pa_xstrdup(sink->name);
        change_list[change_list_size].port_name = pa_xstrdup(port);
        change_list[change_list_size].refresh = refresh;
        change_list_size++;

        return ret;
    }

    return set_port(sink, port, refresh);
}

static void set_port_end(struct userdata *u) {
    if (change_list_size == 0)
        return;

    pa_assert(!delayed_port_change_event);
    delayed_port_change_event = pa_core_rttime_new(u->core, pa_rtclock_now() + 1 * PA_USEC_PER_SEC, delay_cb, u);
}

int pa_sink_ext_set_ports(struct userdata *u, const char *type)
{
    int ret = 0;
    pa_sink *sink;
    struct pa_classify_device_data *data;
    struct pa_classify_port_entry *port_entry;
    char *port;
    struct pa_sink_ext *ext;
    uint32_t idx;

    pa_assert(u);
    pa_assert(u->core);

    set_port_start(u);

    PA_IDXSET_FOREACH(sink, u->core->sinks, idx) {
        /* Check whether the port of this sink should be changed. */
        if (pa_classify_is_port_sink_typeof(u, sink, type, &data)) {

            pa_assert_se(port_entry = pa_hashmap_get(data->ports, sink->name));
            pa_assert_se(port = port_entry->port_name);

            ext  = pa_sink_ext_lookup(u, sink);
            if (!ext)
                continue;

            if (ext->overridden_port) {
                pa_xfree(ext->overridden_port);
                ext->overridden_port = pa_xstrdup(port);
                continue;
            }

            if (!sink->active_port || !pa_streq(port,sink->active_port->name)){
                if (!ext->overridden_port) {
                    ret = set_port_add(u, sink, port, data->flags & PA_POLICY_DELAYED_PORT_CHANGE, false);
                }
                continue;
            }

            if ((data->flags & PA_POLICY_REFRESH_PORT_ALWAYS) && !ext->overridden_port) {
                ret = set_port_add(u, sink, port, data->flags & PA_POLICY_DELAYED_PORT_CHANGE, true);
                continue;
            }

        }
    } /* for */

    set_port_end(u);

    return ret;
}

void pa_sink_ext_set_volumes(struct userdata *u)
{
    struct pa_sink     *sink;
    struct pa_sink_ext *ext;
    uint32_t            idx;

    pa_assert(u);
    pa_assert(u->core);

    PA_IDXSET_FOREACH(sink, u->core->sinks, idx) {
        ext = pa_sink_ext_lookup(u, sink);

        pa_assert(ext);

        if (ext->need_volume_setting) {
            pa_log_debug("set sink '%s' volume", pa_sink_ext_get_name(sink));
            pa_sink_set_volume(sink, NULL, true, false);
            ext->need_volume_setting = false;
        }
    }
}

void pa_sink_ext_override_port(struct userdata *u, struct pa_sink *sink,
                               char *port)
{
    struct pa_sink_ext *ext;
    const char         *name;
    uint32_t            idx;
    char               *active_port;

    if (!sink || !u || !port)
        return;

    name = pa_sink_ext_get_name(sink);
    idx  = sink->index;
    ext  = pa_sink_ext_lookup(u, sink);

    if (ext == NULL) {
        pa_log("no extension found for sink '%s' (idx=%u)", name, idx);
        return;
    }

    active_port = sink->active_port ? sink->active_port->name : "";

    if (ext->overridden_port) {
        if (strcmp(port, active_port)) {
            pa_log_debug("attempt to multiple time to override "
                         "port on sink '%s'", name);
        }
    }
    else {
        ext->overridden_port = pa_xstrdup(active_port);

        if (strcmp(port, active_port)) {
            if (pa_sink_set_port(sink, port, false) < 0)
                pa_log("failed to override sink '%s' port to '%s'", name,port);
            else
                pa_log_debug("overrode sink '%s' port to '%s'", name, port);
        }
    }
}

void pa_sink_ext_restore_port(struct userdata *u, struct pa_sink *sink)
{
    struct pa_sink_ext *ext;
    const char         *name;
    uint32_t            idx;
    const char         *active_port;
    char               *overridden_port;

    if (!sink || !u)
        return;

    name = pa_sink_ext_get_name(sink);
    idx  = sink->index;
    ext  = pa_sink_ext_lookup(u, sink);

    if (ext == NULL) {
        pa_log("no extension found for sink '%s' (idx=%u)", name, idx);
        return;
    }

    active_port     = sink->active_port ? sink->active_port->name : "";
    overridden_port = ext->overridden_port;

    if (overridden_port) {
        if (strcmp(overridden_port, active_port)) {
            if (pa_sink_set_port(sink, overridden_port, false) < 0) {
                pa_log("failed to restore sink '%s' port to '%s'",
                       name, overridden_port);
            }
            else {
                pa_log_debug("restore sink '%s' port to '%s'",
                             name, overridden_port);
            }
        }

        pa_xfree(overridden_port);
        ext->overridden_port = NULL;
    }
}

static pa_hook_result_t sink_put(void *hook_data, void *call_data,
                                 void *slot_data)
{
    struct pa_sink  *sink = (struct pa_sink *)call_data;
    struct userdata *u    = (struct userdata *)slot_data;

    handle_new_sink(u, sink);

    return PA_HOOK_OK;
}


static pa_hook_result_t sink_unlink(void *hook_data, void *call_data,
                                    void *slot_data)
{
    struct pa_sink  *sink = (struct pa_sink *)call_data;
    struct userdata *u    = (struct userdata *)slot_data;

    handle_removed_sink(u, sink);

    return PA_HOOK_OK;
}


static void handle_new_sink(struct userdata *u, struct pa_sink *sink)
{
    const char *name;
    uint32_t  idx;
    char      buf[1024];
    int       len;
    int       ret;
    int       is_null_sink;
    struct pa_null_sink *ns;
    struct pa_sink_ext  *ext;

    if (sink && u) {
        name = pa_sink_ext_get_name(sink);
        idx  = sink->index;
        len  = pa_classify_sink(u, sink, 0,0, buf, sizeof(buf));
        ns   = u->nullsink;

        if (strcmp(name, ns->name))
            is_null_sink = false;
        else {
            ns->sink = sink;
            pa_log_debug("new sink '%s' (idx=%d) will be used to "
                         "mute-by-route", name, idx);
            is_null_sink = true;
        }

        pa_policy_context_register(u, pa_policy_object_sink, name, sink);
        pa_policy_activity_register(u, pa_policy_object_sink, name, sink);

        if (len <= 0) {
            if (!is_null_sink)
                pa_log_debug("new sink '%s' (idx=%d)", name, idx);
        }
        else {
            ret = pa_proplist_sets(sink->proplist,
                                   PA_PROP_POLICY_DEVTYPELIST, buf);

            if (ret < 0) {
                pa_log("failed to set property '%s' on sink '%s'",
                       PA_PROP_POLICY_DEVTYPELIST, name);
            }
            else {
                pa_log_debug("new sink '%s' (idx=%d) (type %s)",
                             name, idx, buf);

                ext = pa_xmalloc0(sizeof(struct pa_sink_ext));
                pa_index_hash_add(u->hsnk, idx, ext);

                pa_policy_groupset_update_default_sink(u, PA_IDXSET_INVALID);
                pa_policy_groupset_register_sink(u, sink);

                len = pa_classify_sink(u, sink, PA_POLICY_DISABLE_NOTIFY,0,
                                       buf, sizeof(buf));
                if (len > 0) {
                    pa_policy_send_device_state(u, PA_POLICY_CONNECTED, buf);
                }
            }
        }
    }
}

static void handle_removed_sink(struct userdata *u, struct pa_sink *sink)
{
    const char          *name;
    uint32_t             idx;
    char                 buf[1024];
    int                  len;
    struct pa_null_sink *ns;
    struct pa_sink_ext  *ext;

    if (sink && u) {
        name = pa_sink_ext_get_name(sink);
        idx  = sink->index;
        len  = pa_classify_sink(u, sink, 0,0, buf, sizeof(buf));
        ns   = u->nullsink;

        if (ns->sink == sink) {
            pa_log_debug("cease to use sink '%s' (idx=%u) to mute-by-route",
                         name, idx);

            /* TODO: move back the streams of this sink to their
               original place */

            ns->sink = NULL;
        }

        pa_policy_context_unregister(u, pa_policy_object_sink, name, sink,idx);
        pa_policy_activity_unregister(u, pa_policy_object_sink, name, sink,idx);

        if (len <= 0)
            pa_log_debug("remove sink '%s' (idx=%u)", name, idx);
        else {
            pa_log_debug("remove sink '%s' (idx=%d, type=%s)", name,idx, buf);

            pa_policy_groupset_update_default_sink(u, idx);
            pa_policy_groupset_unregister_sink(u, idx);

            if ((ext = pa_index_hash_remove(u->hsnk, idx)) == NULL)
                pa_log("no extension found for sink '%s' (idx=%u)",name,idx);
            else {
                pa_xfree(ext->overridden_port);
                pa_xfree(ext);
            }

            len = pa_classify_sink(u, sink, PA_POLICY_DISABLE_NOTIFY,0,
                                   buf, sizeof(buf));
            
            if (len > 0) {
                pa_policy_send_device_state(u, PA_POLICY_DISCONNECTED, buf);
            }
        }
    }
}

void pa_policy_send_device_state(struct userdata *u, const char *state,
                                 char *typelist)
{
#define MAX_TYPE 256

    const char *types[MAX_TYPE];
    int   ntype;
    char  buf[1024];
    char *p, *q, c;

    if (typelist && typelist[0]) {

        ntype = 0;
        
        p = typelist - 1;
        q = buf;
        
        do {
            p++;
            
            if (ntype < MAX_TYPE)
                types[ntype] = q;
            else {
                pa_log("%s() list overflow", __FUNCTION__);
                return;
            }
            
            while ((c = *p) != ' ' && c != '\0') {
                if (q < buf + sizeof(buf)-1)
                    *q++ = *p++;
                else {
                    pa_log("%s() buffer overflow", __FUNCTION__);
                    return;
                }
            }
            *q++ = '\0';
            ntype++;
            
        } while (*p);
        
        pa_policy_dbusif_send_device_state(u, state, types, ntype);
    }

#undef MAX_TYPE
}


/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
