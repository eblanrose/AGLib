#ifndef AGTHREAD_H
#define AGTHREAD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ag_mutex  ag_mutex;
typedef struct ag_thread ag_thread;

ag_mutex *ag_mutex_new(void);
void      ag_mutex_free(ag_mutex *m);
void      ag_mutex_lock(ag_mutex *m);
void      ag_mutex_unlock(ag_mutex *m);

typedef void (*ag_thread_fn)(void *ud);

ag_thread *ag_thread_start(ag_thread_fn fn, void *ud);
void       ag_thread_join(ag_thread *t);

typedef void (*ag_work_fn)(void *ud, int index, int count);
int ag_parallel_for(int count, ag_work_fn fn, void *ud);

int ag_cpu_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AGTHREAD_H */
