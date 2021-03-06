#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/def.h>
#include <pulsecore/module.h>

#include "module-ext.h"
#include "context.h"

#define HASH_INDEX_BITS    8
#define HASH_INDEX_GAP     2
#define HASH_INDEX_MAX     (1 << HASH_INDEX_BITS)
#define HASH_INDEX_MASK    (HASH_INDEX_MAX - 1)
#define HASH_TABLE_SIZE    (HASH_INDEX_MAX << HASH_INDEX_GAP)
#define HASH_TABLE_MASK    (HASH_TABLE_SIZE - 1)
#define HASH_SEARCH_MAX    HASH_INDEX_MAX

#define HASH_INDEX(i)      (((i) & HASH_INDEX_MASK) << HASH_INDEX_GAP)
#define HASH_INDEX_NEXT(i) (((i) + 1) & HASH_TABLE_MASK)


struct hash_entry {
    unsigned long      index;
    struct pa_module  *module;
};


struct hash_entry  hash_table[HASH_TABLE_SIZE];


static void handle_module_events(pa_core *, pa_subscription_event_type_t,
                                 uint32_t, void *);
static void handle_new_module(struct userdata *, struct pa_module *);
static void handle_removed_module(struct userdata *, unsigned long);

static int hash_add(struct pa_module *);
static int hash_delete(unsigned long);


struct pa_module_evsubscr *pa_module_ext_subscription(struct userdata *u)
{
    struct pa_module_evsubscr *subscr;

    pa_assert(u);
    pa_assert(u->core);

    subscr = pa_xnew0(struct pa_module_evsubscr, 1);

    subscr->ev = pa_subscription_new(u->core, 1<<PA_SUBSCRIPTION_EVENT_MODULE,
                                     handle_module_events, (void *)u);

    return subscr;
}

void pa_module_ext_subscription_free(struct pa_module_evsubscr *subscr)
{
    pa_assert(subscr);

    pa_subscription_free(subscr->ev);
}


void pa_module_ext_discover(struct userdata *u)
{
    void             *state = NULL;
    pa_idxset        *idxset;
    struct pa_module *module;

    pa_assert(u);
    pa_assert(u->core);
    pa_assert_se((idxset = u->core->modules));

    while ((module = pa_idxset_iterate(idxset, &state, NULL)) != NULL) {
        hash_add(module);
        handle_new_module(u, module);
    }
}

const char *pa_module_ext_get_name(struct pa_module *module)
{
    return module->name ? module->name : "<unknown>";
}

static void handle_module_events(pa_core *c, pa_subscription_event_type_t t,
                                 uint32_t idx, void *userdata)
{
    struct userdata    *u  = userdata;
    uint32_t            et = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    struct pa_module   *module;
    const char         *name;

    pa_assert(u);
    
    switch (et) {

    case PA_SUBSCRIPTION_EVENT_NEW:
        if ((module = pa_idxset_get_by_index(c->modules, idx)) != NULL) {
            name = pa_module_ext_get_name(module);

            if (hash_add(module)) {
                pa_log_debug("new module #%d  '%s'", idx, name);
                handle_new_module(u, module);
            }
        }
        break;
        
    case PA_SUBSCRIPTION_EVENT_REMOVE:
        if (hash_delete(idx)) {
            pa_log_debug("remove module #%d", idx);
            handle_removed_module(u, idx);
        }
        break;

    default:
        break;
    }
}

static void handle_new_module(struct userdata *u, struct pa_module *module)
{
    const char *name;

    if (module && u) {
        name = pa_module_ext_get_name(module);

        pa_policy_context_register(u, pa_policy_object_module, name, module);
    }
}

static void handle_removed_module(struct userdata *u, unsigned long idx)
{
    char name[256];

    if (u) {

        snprintf(name, sizeof(name), "module #%lu", idx);

        pa_policy_context_unregister(u, pa_policy_object_module,
                                     name, NULL, idx);
    }
}


static int hash_add(struct pa_module *module)
{
    int hidx = HASH_INDEX(module->index);
    int i;
    

    for (i = 0;   i < HASH_SEARCH_MAX;   i++) {

        if (hash_table[hidx].module == NULL) {
            hash_table[hidx].index  = module->index;
            hash_table[hidx].module = module;
            return true;
        }

        if (hash_table[hidx].module == module)
            break;
    }

    return false;
}

static int hash_delete(unsigned long index)
{
    int hidx = HASH_INDEX(index);
    int i;
    

    for (i = 0;   i < HASH_SEARCH_MAX;   i++) {
        if (hash_table[hidx].index == index) {
            hash_table[hidx].index  = 0;
            hash_table[hidx].module = NULL;
            return true;
        }

        hidx = HASH_INDEX_NEXT(hidx);
    }

    return false;
}


/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
