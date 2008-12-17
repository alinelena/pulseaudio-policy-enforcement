
#include <pulsecore/pulsecore-config.h>

#include <pulsecore/namereg.h>
#include <pulse/volume.h>

#include "policy-group.h"
#include "sink-ext.h"
#include "source-ext.h"
#include "sink-input-ext.h"
#include "source-output-ext.h"
#include "classify.h"


struct target {
    enum pa_policy_route_class  class;
    union {
        void                   *any;
        struct pa_sink         *sink;
        struct pa_source       *source;
    };
};


struct cursor {
    int idx;
    struct pa_policy_group *grp;
};


static struct pa_sink   *defsink;
static struct pa_source *defsource;
static uint32_t          defsinkidx = PA_IDXSET_INVALID;
static uint32_t          defsrcidx  = PA_IDXSET_INVALID;

static int move_group(struct pa_policy_group *, struct target *);
static int volset_group(struct pa_policy_group *, pa_volume_t);
static int cork_group(struct pa_policy_group *, int);

static struct pa_policy_group *find_group_by_name(struct pa_policy_groupset *,
                                                  char *, uint32_t *);

static struct pa_sink   *find_sink_by_type(struct userdata *, char *);
static struct pa_source *find_source_by_type(struct userdata *, char *);

static uint32_t hash_value(char *);


struct pa_policy_groupset *pa_policy_groupset_new(struct userdata *u)
{
    struct pa_policy_groupset *gset;

    pa_assert(u);
    
    gset = pa_xnew0(struct pa_policy_groupset, 1);

    return gset;
}

void pa_policy_groupset_free(struct pa_policy_groupset *gset)
{
    pa_assert(gset);

    pa_xfree(gset);
}

void pa_policy_groupset_update_default_sink(struct userdata *u, uint32_t idx)
{
    struct pa_policy_groupset *gset;
    struct pa_policy_group    *group;
    char                      *defsinkname;
    int                        i;

    pa_assert(u);
    pa_assert((gset = u->groups));

    /*
     * Remove the sink from all groups if idx were specified
     * and equals to the default sink's index
     */

    if (defsink != NULL && defsinkidx == idx) {
        pa_log_debug("Unset default sink (idx=%d)", idx);

        for (i = 0;   i < PA_POLICY_GROUP_HASH_DIM;   i++) {
            for (group = gset->hash_tbl[i]; group; group = group->next) {
                if (group->sinkidx == defsinkidx) {
                    pa_log_debug("  unset default sink for group '%s'",
                                 group->name);
                    group->sink = NULL;
                    group->sinkidx = PA_IDXSET_INVALID;
                }
            }
        }
        
        defsink = NULL;
        defsinkidx = PA_IDXSET_INVALID;
    }

    /*
     * Try to find the default sink if we do not had any
     */

    if (defsink == NULL) {
        defsink = pa_namereg_get(u->core, NULL, PA_NAMEREG_SINK, FALSE);

        if (defsink != NULL) {
            defsinkname = pa_sink_ext_get_name(defsink);
            defsinkidx  = defsink->index;

            pa_log_debug("Set default sink to '%s' (idx=%d)",
                         defsinkname, defsinkidx);

            for (i = 0;   i < PA_POLICY_GROUP_HASH_DIM;   i++) {
                for (group = gset->hash_tbl[i]; group; group = group->next) {
                    if (group->sinkname == NULL && group->sink == NULL) {
                        pa_log_debug("  set sink '%s' as default for "
                                     "group '%s'", defsinkname, group->name);
                        group->sink = defsink;
                        group->sinkidx = defsinkidx;

                        /* TODO: we should move the streams to defsink */
                    }
                }
            }
        }
    }
}


void pa_policy_groupset_register_sink(struct userdata *u, struct pa_sink *sink)
{
    struct pa_policy_groupset *gset;
    struct pa_policy_group    *group;
    char                      *sinkname;
    uint32_t                   sinkidx;
    int                        i;

    pa_assert(u);
    pa_assert(sink);
    pa_assert((gset = u->groups));

    sinkname = pa_sink_ext_get_name(sink);
    sinkidx  = sink->index;

    if (sinkname && sinkname[0]) {
        pa_log_debug("Register sink '%s' (idx=%d)", sinkname, sinkidx);
        
        for (i = 0;   i < PA_POLICY_GROUP_HASH_DIM;   i++) {
            for (group = gset->hash_tbl[i];    group;    group = group->next) {
                if (group->sinkname && !strcmp(group->sinkname, sinkname)) {
                    pa_log_debug("  set sink '%s' as default for group '%s'",
                                 sinkname, group->name);

                    group->sink    = sink;
                    group->sinkidx = sinkidx;

                    /* TODO: we should move the streams to the sink */
                }
            }
        }
    }
}

void pa_policy_groupset_unregister_sink(struct userdata *u, uint32_t sinkidx)
{
    struct pa_policy_groupset *gset;
    struct pa_policy_group    *group;
    int                        i;

    pa_assert(u);
    pa_assert((gset = u->groups));

    pa_log_debug("Unregister sink (idx=%d)", sinkidx);
        
    for (i = 0;   i < PA_POLICY_GROUP_HASH_DIM;   i++) {
        for (group = gset->hash_tbl[i];    group;    group = group->next) {
            if (group->sinkidx == sinkidx) {
                pa_log_debug("  unset default sink for group '%s'",
                             group->name);

                group->sink    = NULL;
                group->sinkidx = PA_IDXSET_INVALID;

                /* TODO: we should move the streams to somewhere */
            }
        }
    }
}

void pa_policy_groupset_register_source(struct userdata *u,
                                        struct pa_source *source)
{
    struct pa_policy_groupset *gset;
    struct pa_policy_group    *group;
    char                      *srcname;
    uint32_t                   srcidx;
    int                        i;

    pa_assert(u);
    pa_assert(source);
    pa_assert((gset = u->groups));

    srcname = pa_source_ext_get_name(source);
    srcidx  = source->index;

    if (srcname && srcname[0]) {
        pa_log_debug("Register source '%s' (idx=%d)", srcname, srcidx);
        
        for (i = 0;   i < PA_POLICY_GROUP_HASH_DIM;   i++) {
            for (group = gset->hash_tbl[i];    group;    group = group->next) {
                if (group->srcname && !strcmp(group->srcname, srcname)) {
                    pa_log_debug("  set source '%s' as default for group '%s'",
                                 srcname, group->name);

                    group->source = source;
                    group->srcidx = srcidx;

                    /* TODO: we should move the streams to the source */
                }
            }
        }
    }
}

void pa_policy_groupset_unregister_source(struct userdata *u, uint32_t srcidx)
{
    struct pa_policy_groupset *gset;
    struct pa_policy_group    *group;
    int                        i;

    pa_assert(u);
    pa_assert((gset = u->groups));

    pa_log_debug("Unregister source (idx=%d)", srcidx);
        
    for (i = 0;   i < PA_POLICY_GROUP_HASH_DIM;   i++) {
        for (group = gset->hash_tbl[i];    group;    group = group->next) {
            if (group->srcidx == srcidx) {
                pa_log_debug("  unset default source for group '%s'",
                             group->name);

                group->source = NULL;
                group->srcidx = PA_IDXSET_INVALID;
                
                /* TODO: we should move the streams to the somwhere */
            }
        }
    }
}

void pa_policy_groupset_create_default_group(struct userdata *u)
{
    static char     *name = (char *)PA_POLICY_DEFAULT_GROUP_NAME;
    static uint32_t flags = PA_POLICY_GROUP_FLAGS_CLIENT;

    struct pa_policy_groupset *gset;
    
    pa_assert(u);
    pa_assert((gset = u->groups));

    gset->dflt = pa_policy_group_new(u, name, NULL, NULL, flags);
}



struct pa_policy_group *pa_policy_group_new(struct userdata *u, char *name, 
                                            char *sinkname, char *srcname,
                                            uint32_t flags)
{
    struct pa_policy_groupset *gset;
    struct pa_policy_group    *group;
    uint32_t                   idx;

    pa_assert(u);
    pa_assert((gset = u->groups));

    if ((group = find_group_by_name(gset, name, &idx)) != NULL)
        return group;

    group = pa_xnew0(struct pa_policy_group, 1);

    group->next     = gset->hash_tbl[idx];
    group->flags    = flags;
    group->name     = pa_xstrdup(name);
    group->limit    = PA_VOLUME_NORM;
    group->sinkname = sinkname ? pa_xstrdup(sinkname) : NULL;
    group->sink     = sinkname ? defsink : NULL;
    group->sinkidx  = sinkname ? defsinkidx : PA_IDXSET_INVALID;
    group->srcname  = srcname  ? pa_xstrdup(srcname) : NULL;
    group->source   = srcname  ? defsource : NULL;
    group->srcidx   = srcname  ? defsrcidx : PA_IDXSET_INVALID;

    gset->hash_tbl[idx] = group;

    pa_log_info("created group (%s|%d|%s|0x%04x)", group->name,
                (group->limit * 100) / PA_VOLUME_NORM,
                group->sink?group->sink->name:"<null>",
                group->flags);

    return group;
}

void pa_policy_group_free(struct pa_policy_groupset *gset, char *name)
{
    struct pa_policy_group       *group;
    struct pa_policy_group       *dflt;
    struct pa_policy_group       *prev;
    struct pa_sink_input         *sinp;
    struct pa_sink_input_list    *sil;
    struct pa_sink_input_list    *nxtsi;
    struct pa_source_output      *sout;
    struct pa_source_output_list *sol;
    struct pa_source_output_list *nxtso;
    char                         *dnam;
    uint32_t                      idx;

    pa_assert(gset);
    pa_assert(name);

    if ((group = find_group_by_name(gset, name, &idx)) != NULL) {
        for (prev = (struct pa_policy_group *)&gset->hash_tbl[idx];
             prev->next != NULL;
             prev = prev->next)
        {
            if (group == prev->next) {
                if (group->sinpls != NULL) {
                    dflt = gset->dflt;

                    if (group == dflt) {
                        /*
                         * If the default group is going to be deleted,
                         * release all sink-inputs
                         */
                        for (sil = group->sinpls;   sil;   sil = nxtsi) {
                            nxtsi = sil->next;
                            sinp  = sil->sink_input;

                            pa_sink_input_ext_set_policy_group(sinp, NULL);

                            pa_xfree(sil);
                        }
                    }
                    else {
                        /*
                         * Otherwise add the sink-inputs to the default group
                         */
                        dnam = dflt->name;

                        for (sil = group->sinpls;   sil;   sil = sil->next) {
                            sinp = sil->sink_input;

                            pa_sink_input_ext_set_policy_group(sinp, dnam);
                            
                            if (sil->next == NULL)
                                sil->next = dflt->sinpls;
                        }
                        
                        dflt->sinpls = group->sinpls;
                    }
                } /* if group->sinpls != NULL */

                if (group->soutls != NULL) {
                    for (sol = group->soutls;  sol;  sol = nxtso) {
                        nxtso = sol->next;
                        sout  = sol->source_output;

                        pa_source_output_ext_set_policy_group(sout, NULL);

                        pa_xfree(sol);
                    }
                } /* if group->soutls */

                pa_xfree(group->name);
                pa_xfree(group->sinkname);
                pa_xfree(group->srcname);

                prev->next = group->next;

                pa_xfree(group);

                break;
            } 
        } /* for */
    } /* if find_group */
}

struct pa_policy_group *pa_policy_group_find(struct userdata *u, char *name)
{
    struct pa_policy_groupset *gset;

    assert(u);
    assert((gset = u->groups));
    assert(name);

    return find_group_by_name(gset, name, NULL);
}

void pa_policy_group_insert_sink_input(struct userdata      *u,
                                       char                 *name,
                                       struct pa_sink_input *si)
{
    struct pa_policy_groupset *gset;
    struct pa_policy_group    *group;
    struct pa_sink_input_list *sl;


    pa_assert(u);
    pa_assert((gset = u->groups));
    pa_assert(si);

    if (name == NULL)
        group = gset->dflt;
    else
        group = find_group_by_name(gset, name, NULL);

    if (group != NULL) {
        pa_sink_input_ext_set_policy_group(si, group->name);

        sl = pa_xnew0(struct pa_sink_input_list, 1);
        sl->next = group->sinpls;
        sl->index = si->index;
        sl->sink_input = si;

        group->sinpls = sl;

        if (group->sink != NULL) {
            pa_sink_input_move_to(si, group->sink);

            if (group->corked)
                pa_sink_input_cork(si, group->corked);

            pa_sink_input_ext_set_volume_limit(si, group->limit);
        }

        pa_log_debug("sink input '%s' added to group '%s'",
                     pa_sink_input_ext_get_name(si), group->name);
    }
}


void pa_policy_group_remove_sink_input(struct userdata *u, uint32_t idx)
{
    struct pa_policy_group    *group;
    struct pa_sink_input_list *prev;
    struct pa_sink_input_list *sl;
    void                      *cursor = NULL;

    pa_assert(u);
    pa_assert(u->groups);

    while ((group = pa_policy_group_scan(u->groups, &cursor)) != NULL) {
        for (prev = (struct pa_sink_input_list *)&group->sinpls;
             prev != NULL;
             prev = prev->next)
        {
            if ((sl = prev->next) != NULL && idx == sl->index) {
                prev->next = sl->next;

                pa_xfree(sl);

                pa_log_debug("sink input (idx=%d) removed from group '%s'",
                             idx, group->name);

                return;
            }
        }
    }

    pa_log("Can't remove sink input (idx=%d): not a member of any group", idx);
}

void pa_policy_group_insert_source_output(struct userdata         *u,
                                          char                    *name,
                                          struct pa_source_output *so)
{
    struct pa_policy_groupset    *gset;
    struct pa_policy_group       *group;
    struct pa_source_output_list *sl;


    pa_assert(u);
    pa_assert(name);
    pa_assert((gset = u->groups));
    pa_assert(so);

    if ((group = find_group_by_name(gset, name, NULL)) != NULL) {
        pa_source_output_ext_set_policy_group(so, group->name);

        sl = pa_xnew0(struct pa_source_output_list, 1);
        sl->next = group->soutls;
        sl->index = so->index;
        sl->source_output = so;

        group->soutls = sl;

        if (group->source != NULL) {
            pa_source_output_move_to(so, group->source);
        }

       pa_log_debug("source output '%s' added to group '%s'",
                     pa_source_output_ext_get_name(so), group->name);
    }
}


void pa_policy_group_remove_source_output(struct userdata *u, uint32_t idx)
{
    struct pa_policy_group       *group;
    struct pa_source_output_list *prev;
    struct pa_source_output_list *sl;
    void                         *cursor = NULL;

    pa_assert(u);
    pa_assert(u->groups);

    while ((group = pa_policy_group_scan(u->groups, &cursor)) != NULL) {
        for (prev = (struct pa_source_output_list *)&group->soutls;
             prev != NULL;
             prev = prev->next)
        {
            if ((sl = prev->next) != NULL && idx == sl->index) {
                prev->next = sl->next;

                pa_xfree(sl);

                pa_log_debug("source output (idx=%d) removed from group '%s'",
                             idx, group->name);

                return;
            }
        }
    }

    pa_log("Can't remove source output (idx=%d): "
           "not a member of any group", idx);
}

int pa_policy_group_move_to(struct userdata *u, char *name,
                            enum pa_policy_route_class class, char *type)
{
    struct pa_policy_group   *grp;
    struct target             target;
    void                     *curs;
    int                       target_is_sink;
    int                       ret = -1;

    pa_assert(u);

    target.class = class;

    switch (class) {
        
    case pa_policy_route_to_sink:
        target.sink = find_sink_by_type(u, type);
        target_is_sink = TRUE;
        break;

    case pa_policy_route_to_source:
        target.source = find_source_by_type(u, type);
        target_is_sink = FALSE;
        break;

    default:
        target.any = NULL;
        break;
    }


    if (target.any != NULL) {
        if (name) {             /* move the specified group only */
            if ((grp = find_group_by_name(u->groups, name, NULL)) != NULL) {
                if (!(grp->flags & PA_POLICY_GROUP_FLAG_ROUTE_AUDIO))
                    ret = 0;
                else {
                    if ((ret = move_group(grp, &target)) >= 0) {
                        if (target_is_sink)
                            grp->sink = target.sink;
                        else
                            grp->source = target.source;
                    }
                }
            }
        }
        else {                  /* move all groups */
            ret = 0;

            for (curs = NULL; (grp = pa_policy_group_scan(u->groups, &curs));){
                if (!(grp->flags & PA_POLICY_GROUP_FLAG_ROUTE_AUDIO))
                    ret = 0;
                else {
                    if (move_group(grp, &target) < 0)
                        ret = -1;
                    else {
                        if (target_is_sink)
                            grp->sink = target.sink;
                        else
                            grp->source = target.source;
                    }
                }
            }
        }
    }

    return ret;
}

int pa_policy_group_cork(struct userdata *u, char *name, int corked)
{
    struct pa_policy_group *grp;
    int                     ret;

    pa_assert(u);

    if ((grp = find_group_by_name(u->groups, name, NULL)) == NULL)
        ret = -1;
    else {
        if (!(grp->flags & PA_POLICY_GROUP_FLAG_CORK_STREAM))
            ret = 0;
        else {
            ret = cork_group(grp, corked);
            grp->corked = corked;
        }
    }

    return ret;
}


int pa_policy_group_volume_limit(struct userdata *u, char *name, uint32_t limit)
{
    struct pa_policy_groupset *gset;
    struct pa_policy_group    *group;
    int                        ret;

    pa_assert(u);
    pa_assert((gset = u->groups));

    if (name == NULL)
        group = gset->dflt;
    else
        group = find_group_by_name(gset, name, NULL);

    if (group == NULL) {
        pa_log("%s: can't set volume limit: don't know group '%s'",
               __FILE__, name ? name : PA_POLICY_DEFAULT_GROUP_NAME);
        ret = -1;
    }
    else {
        if (!(group->flags & PA_POLICY_GROUP_FLAG_LIMIT_VOLUME))
            ret = 0;
        else {
            pa_log_debug("%s: setting volume limit %d for group '%s'",
                         __FILE__, limit, group->name);
            ret = volset_group(group, limit);
            group->limit = limit;
        }
    }

    return ret;
}

struct pa_policy_group *pa_policy_group_scan(struct pa_policy_groupset *gset,
                                             void **pcursor)
{
    struct cursor *cursor;
    struct pa_policy_group *grp;

    pa_assert(gset);
    pa_assert(pcursor);

    if ((cursor = *pcursor) == NULL) {
        cursor = pa_xnew0(struct cursor, 1);
        *pcursor = cursor;
    }

    
    for (;;) {
        if ((grp = cursor->grp) != NULL) {
            cursor->grp = grp->next;
            return grp;
        }

        if (cursor->idx >= PA_POLICY_GROUP_HASH_DIM) {
            pa_xfree(cursor);
            *pcursor = NULL;
            return NULL;
        }

        while (cursor->idx < PA_POLICY_GROUP_HASH_DIM &&
               (cursor->grp = gset->hash_tbl[cursor->idx++]) == NULL)
            ;
    }
}


static int move_group(struct pa_policy_group *group, struct target *target)
{
    struct pa_sink               *sink;
    struct pa_source             *source;
    struct pa_sink_input_list    *sil;
    struct pa_source_output_list *sol;
    struct pa_sink_input         *sinp;
    struct pa_source_output      *sout;
    int                           ret = 0;

    if (group == NULL || target->any == NULL)
        ret = -1;
    else {
        switch (target->class) {
            
        case pa_policy_route_to_sink:
            if ((sink = target->sink) == group->sink) {
                pa_log_debug("group '%s' is aready routed to sink '%s'",
                             group->name, pa_sink_ext_get_name(sink));
            }
            else {
                for (sil = group->sinpls;    sil;   sil = sil->next) {
                    sinp = sil->sink_input;
                    
                    pa_log_debug("move sink input '%s' to sink '%s'",
                                 pa_sink_input_ext_get_name(sinp),
                                 pa_sink_ext_get_name(sink));
                    
                    if (pa_sink_input_move_to(sinp, sink) < 0) {
                        ret = -1;
                        
                        pa_log("failed to move sink input '%s' to sink '%s'",
                               pa_sink_input_ext_get_name(sinp),
                               pa_sink_ext_get_name(sink));
                    }
                }
            }
            break;
            
        case pa_policy_route_to_source:
            if ((source = target->source) == group->source) {
                pa_log_debug("group '%s' is aready routed to source '%s'",
                             group->name, pa_source_ext_get_name(source));
            }
            else {
                for (sol = group->soutls;    sol;    sol = sol->next) {
                    sout = sol->source_output;
                    
                    pa_log_debug("move source output '%s' to source '%s'",
                                 pa_source_output_ext_get_name(sout),
                                 pa_source_ext_get_name(source));
                    
                    if (pa_source_output_move_to(sout, source) < 0) {
                        ret = -1;
                    
                        pa_log("failed to move source output '%s' to source "
                               "'%s'", pa_source_output_ext_get_name(sout),
                               pa_source_ext_get_name(source));
                    }
                }
            }
            break;
            
        default:
            ret = -1;
            break;
        } /* switch class */
    }

    return ret;
}


static int volset_group(struct pa_policy_group *group, pa_volume_t limit)
{
    struct pa_sink_input_list *sl;
    struct pa_sink_input *sinp;
    int ret = 0;


    limit = ((limit > 100 ? 100 : limit) * PA_VOLUME_NORM) / 100;

    if (limit == group->limit) {
        pa_log_debug("group '%s' volume limit is already %d",
                     group->name, limit);
    }
    else {
        for (sl = group->sinpls;   sl != NULL;   sl = sl->next) {
            sinp = sl->sink_input;

            if (pa_sink_input_ext_set_volume_limit(sinp, limit) < 0)
                ret = -1;
            else
                pa_log_debug("set volume limit %d for stream '%s'",
                             limit, pa_sink_input_ext_get_name(sinp));
        }
    }

    return ret;
}


static int cork_group(struct pa_policy_group *group, int corked)
{
    struct pa_sink_input_list *sl;
    struct pa_sink_input *sinp;


    if (corked == group->corked) {
        pa_log_debug("group '%s' is already %s", group->name,
                     corked ? "corked" : "uncorked");
    }
    else {
        for (sl = group->sinpls;    sl;   sl = sl->next) {
            sinp = sl->sink_input;
            
            pa_sink_input_cork(sinp, corked);
            
            pa_log_debug("sink input '%s' %s",
                         pa_sink_input_ext_get_name(sinp),
                         corked ? "corked" : "uncorked");
        }
    }

    return 0;
}


static struct pa_policy_group *
find_group_by_name(struct pa_policy_groupset *gset, char *name, uint32_t *ridx)
{
    struct pa_policy_group *group = NULL;
    uint32_t                idx   = hash_value(name);
    
    pa_assert(gset);
    pa_assert(name);

    for (group = gset->hash_tbl[idx];   group != NULL;   group = group->next) {
        if (!strcmp(name, group->name))
            break;
    }    

    if (ridx != NULL)
        *ridx = idx;

    return group;
}


static struct pa_sink *find_sink_by_type(struct userdata *u, char *type)
{
    void            *state = NULL;
    pa_idxset       *idxset;
    struct pa_sink  *sink;

    pa_assert(u);
    pa_assert(type);
    pa_assert(u->core);
    pa_assert((idxset = u->core->sinks));

    while ((sink = pa_idxset_iterate(idxset, &state, NULL)) != NULL) {
        if (pa_classify_is_sink_typeof(u, sink, type))
            break;
    }

    return sink;
}

static struct pa_source *find_source_by_type(struct userdata *u, char *type)
{
    void              *state = NULL;
    pa_idxset         *idxset;
    struct pa_source  *source;

    pa_assert(u);
    pa_assert(type);
    pa_assert(u->core);
    pa_assert((idxset = u->core->sources));

    while ((source = pa_idxset_iterate(idxset, &state, NULL)) != NULL) {
        if (pa_classify_is_source_typeof(u, source, type))
            break;
    }

    return source;
}

static uint32_t hash_value(char *s)
{
    uint32_t hash = 0;
    unsigned char c;

    if (s) {
        while ((c = *s++) != '\0') {
            hash = 38501 * (hash + c);
        }
    }

    return hash & PA_POLICY_GROUP_HASH_MASK;
}


/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
