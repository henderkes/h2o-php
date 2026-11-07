#ifndef HP_INPUT_RETURN_H
#define HP_INPUT_RETURN_H

#include "h2o_php.h"

/* Origin-loop-only weak token. Its live generator holds the native request
 * reference and invalidates this token before releasing it. The worker never
 * reads it, and the token itself must not retain an unread owning request. */
typedef struct {
    hp_request_t *request;
} hp_input_origin_t;

static inline void hp_input_origin_invalidate(hp_input_origin_t *origin)
{
    origin->request = NULL;
}

static inline hp_request_t *hp_input_origin_acquire(const hp_input_origin_t *origin)
{
    hp_request_t *request = origin->request;
    if (request) hp_request_retain(request);
    return request;
}

#endif
