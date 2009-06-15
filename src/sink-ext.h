#ifndef foosinkextfoo
#define foosinkextfoo

#include "userdata.h"

struct pa_sink;

struct pa_null_sink {
    char            *name;
    struct pa_sink  *sink;
};

struct pa_sink_evsubscr {
    pa_hook_slot    *put;
    pa_hook_slot    *unlink;
};

struct pa_null_sink *pa_sink_ext_init_null_sink(char *);
void pa_sink_ext_null_sink_free(struct pa_null_sink *);
struct pa_sink_evsubscr *pa_sink_ext_subscription(struct userdata *);
void  pa_sink_ext_subscription_free(struct pa_sink_evsubscr *);
void  pa_sink_ext_discover(struct userdata *);
char *pa_sink_ext_get_name(struct pa_sink *);

void pa_policy_send_device_state(struct userdata *, const char *, char *);

#endif /* foosinkextfoo */

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
