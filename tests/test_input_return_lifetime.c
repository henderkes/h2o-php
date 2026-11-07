/* Exercise the actual origin token with real unread bridge ownership. */
#include "../src/bridge/bridge.c"
#include "../src/h2o/input_return.h"
#include <assert.h>

hp_runtime_config_t hp_config = {.max_buffered_body=1048576, .max_buffered_response=16384,
    .max_buffered_global=8388608, .request_timeout_ms=2000};
void hp_php_metrics_request_cancel_locked(hp_php_metric_request_t *r, uint64_t t) {(void)r;(void)t;}
void hp_php_metrics_request_end_locked(hp_php_metric_request_t *r, uint64_t t) {(void)r;(void)t;}

struct owner { hp_input_origin_t origin; char *data; size_t capacity; unsigned returns; };
static struct owner *pending;
static void returned(void *data, size_t capacity)
{
    struct owner *owner=data;
    assert(owner->returns++==0 && pending==NULL);
    owner->capacity=capacity;
    pending=owner;
}
static struct owner *adopt(hp_request_t *request)
{
    struct owner *owner=calloc(1,sizeof(*owner)); assert(owner);
    owner->data=malloc(524288); assert(owner->data); memset(owner->data,'L',262144);
    owner->origin.request=request;
    assert(atomic_load(&request->refs)==1);
    assert(hp_request_body_push_owned(request,owner,owner->data,262144,524288,returned,false)==1);
    assert(atomic_load(&request->refs)==1); /* the weak token must not form a cycle */
    return owner;
}
static void drain(struct owner *owner)
{
    assert(pending==owner && owner->returns==1 && hp_bridge_buffered_bytes()==524288);
    pending=NULL;
    free(owner->data);
    hp_request_body_release_owned_capacity(owner->capacity);
    free(owner);
    assert(hp_bridge_buffered_bytes()==0);
}
int main(void)
{
    /* Close invalidates before releasing the generator's last ref. With no
     * finish and no reads, destruction must still publish exactly one return. */
    hp_request_t *request=hp_request_new(); struct owner *owner=adopt(request);
    hp_input_origin_invalidate(&owner->origin);
    hp_request_release(request);
    assert(hp_input_origin_acquire(&owner->origin)==NULL);
    drain(owner);

    request=hp_request_new(); owner=adopt(request);
    hp_request_cancel(request);
    hp_input_origin_invalidate(&owner->origin);
    hp_request_release(request);
    assert(hp_input_origin_acquire(&owner->origin)==NULL);
    drain(owner);

    /* A live origin callback takes only a temporary ref. A synchronous close
     * during proceed may drop the generator ref without invalidating r. */
    request=hp_request_new(); owner=adopt(request);
    char *out=malloc(262144); assert(out);
    assert(hp_request_body_read(request,out,262144)==262144 && out[0]=='L'); free(out);
    hp_request_t *temporary=hp_input_origin_acquire(&owner->origin);
    assert(temporary==request && atomic_load(&request->refs)==2);
    hp_input_origin_invalidate(&owner->origin);
    hp_request_release(request);
    assert(atomic_load(&temporary->refs)==1 && !temporary->body_eof);
    drain(owner);
    hp_request_release(temporary);
    hp_bridge_cleanup();
    puts("PASS: unread/no-finish destruction, stale close/cancel return, temporary callback lifetime, one-time physical-capacity drain");
}
