#ifndef __ACTION_1__
#define __ACTION_1__

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*act_1)(void *obj);

typedef struct {
    void *obj;
    act_1 act;
} action_1;

static inline void action_1_perf(action_1 action)
{
    action.act(action.obj);
}

extern action_1 NULL_ACTION_1;

#ifdef __cplusplus
}
#endif

#endif
