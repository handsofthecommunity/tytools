/* TyTools - public domain
   Niels Martignène <niels.martignene@gmail.com>
   https://neodd.com/tytools

   This software is in the public domain. Where that dedication is not
   recognized, you are granted a perpetual, irrevocable license to copy,
   distribute, and modify this file as you see fit.

   See the LICENSE file for more details. */

#include "common_priv.h"
#include "list.h"
#include "system.h"
#include "task.h"

struct ty_pool {
    int unused_timeout;
    unsigned int max_threads;

    ty_mutex mutex;

    ty_list_head threads;
    unsigned int started;
    unsigned int busy;

    ty_list_head pending_tasks;
    ty_cond pending_cond;

    bool init;
};

struct pool_thread {
    ty_pool *pool;
    ty_list_head list;

    ty_thread thread;
    bool run;
};

static ty_pool *default_pool;
static TY_THREAD_LOCAL ty_task *current_task;

int ty_pool_new(ty_pool **rpool)
{
    assert(rpool);

    ty_pool *pool;
    int r;

    pool = calloc(1, sizeof(*pool));
    if (!pool) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }

    pool->max_threads = 16;
    pool->unused_timeout = 10000;

    r = ty_mutex_init(&pool->mutex);
    if (r < 0)
        goto error;
    r = ty_cond_init(&pool->pending_cond);
    if (r < 0)
        goto error;

    ty_list_init(&pool->threads);
    ty_list_init(&pool->pending_tasks);

    pool->init = true;

    *rpool = pool;
    return 0;

error:
    ty_pool_free(pool);
    return r;
}

void ty_pool_free(ty_pool *pool)
{
    if (pool) {
        if (pool->init) {
            ty_mutex_lock(&pool->mutex);

            ty_list_foreach(cur, &pool->pending_tasks) {
                ty_task *task = ty_container_of(cur, ty_task, list);
                ty_task_unref(task);
            }
            ty_list_init(&pool->pending_tasks);
            pool->max_threads = 0;
            ty_cond_broadcast(&pool->pending_cond);

            /* This is a signal for worker threads to stop detaching themselves and
               freeing their own structures, because we need to join with them. */
            pool->init = false;

            ty_mutex_unlock(&pool->mutex);

            ty_list_foreach(cur, &pool->threads) {
                struct pool_thread *thread = ty_container_of(cur, struct pool_thread, list);

                ty_thread_join(&thread->thread);
                free(thread);
            }
        }

        ty_cond_release(&pool->pending_cond);
        ty_mutex_release(&pool->mutex);
    }

    free(pool);
}

static int start_thread(ty_pool *pool);
int ty_pool_set_max_threads(ty_pool *pool, unsigned int max)
{
    assert(pool);

    int r;

    ty_mutex_lock(&pool->mutex);

    if (max > pool->max_threads) {
        ty_list_foreach(cur, &pool->pending_tasks) {
            if (pool->started >= max)
                break;

            r = start_thread(pool);
            if (r < 0) {
                if (pool->started)
                    r = 0;
                goto cleanup;
            }
        }
    } else {
        ty_cond_broadcast(&pool->pending_cond);
    }
    pool->max_threads = max;

    r = 0;
cleanup:
    ty_mutex_unlock(&pool->mutex);
    return r;
}

unsigned int ty_pool_get_max_threads(ty_pool *pool)
{
    assert(pool);
    return pool->max_threads;
}

void ty_pool_set_idle_timeout(ty_pool *pool, int timeout)
{
    assert(pool);

    ty_mutex_lock(&pool->mutex);
    pool->unused_timeout = timeout;
    ty_cond_broadcast(&pool->pending_cond);
    ty_mutex_unlock(&pool->mutex);
}

int ty_pool_get_idle_timeout(ty_pool *pool)
{
    assert(pool);
    return pool->unused_timeout;
}

static void cleanup_default_pool(void)
{
    ty_pool_free(default_pool);
}

int ty_pool_get_default(ty_pool **rpool)
{
    assert(rpool);

    if (!default_pool) {
        int r = ty_pool_new(&default_pool);
        if (r < 0)
            return r;

        atexit(cleanup_default_pool);
    }

    *rpool = default_pool;
    return 0;
}

int ty_task_new(const char *name, int (*run)(ty_task *task), ty_task **rtask)
{
    assert(name);
    assert(run);
    assert(rtask);

    ty_task * task;
    int r;

    task = calloc(1, sizeof(*task));
    if (!task) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }
    task->refcount = 1;

    task->task_run = run;
    task->name = strdup(name);
    if (!task->name) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto error;
    }

    r = ty_mutex_init(&task->mutex);
    if (r < 0)
        goto error;
    r = ty_cond_init(&task->cond);
    if (r < 0)
        goto error;

    *rtask = task;
    return 0;

error:
    ty_task_unref(task);
    return r;
}

ty_task *ty_task_ref(ty_task *task)
{
    assert(task);

    _ty_refcount_increase(&task->refcount);
    return task;
}

void ty_task_unref(ty_task *task)
{
    if (task) {
        if (_ty_refcount_decrease(&task->refcount))
            return;

        if (task->result_cleanup)
            (*task->result_cleanup)(task->result);

        if (task->user_cleanup)
            (*task->user_cleanup)(task->user_cleanup_udata);
        if (task->task_finalize)
            (*task->task_finalize)(task);

        free(task->name);
        ty_cond_release(&task->cond);
        ty_mutex_release(&task->mutex);
    }

    free(task);
}

static void change_status(ty_task *task, ty_task_status status)
{
    ty_message_data msg = {0};

    task->status = status;

    ty_mutex_lock(&task->mutex);
    ty_cond_broadcast(&task->cond);
    ty_mutex_unlock(&task->mutex);

    msg.task = task;
    msg.type = TY_MESSAGE_STATUS;
    msg.u.task.status = status;

    ty_message(&msg);
}

static void run_task(ty_task *task)
{
    assert(task->status <= TY_TASK_STATUS_PENDING);

    ty_task *previous_task;

    previous_task = current_task;
    current_task = task;

    change_status(task, TY_TASK_STATUS_RUNNING);
    task->ret = (*task->task_run)(task);
    if (task->task_finalize) {
        (*task->task_finalize)(task);
        task->task_finalize = NULL;
    }
    change_status(task, TY_TASK_STATUS_FINISHED);

    current_task = previous_task;
}

static int task_thread(void *udata)
{
    struct pool_thread *thread = udata;
    ty_pool *pool = thread->pool;

    while (true) {
        uint64_t start;
        bool run;
        ty_task *task;

        ty_mutex_lock(&pool->mutex);
        pool->busy--;

        run = true;
        start = ty_millis();
        while (true) {
            if (pool->started > pool->max_threads)
                goto timeout;
            task = ty_list_get_first(&pool->pending_tasks, ty_task, list);
            if (task) {
                ty_list_remove(&task->list);
                break;
            }
            if (!run)
                goto timeout;

            run = ty_cond_wait(&pool->pending_cond, &pool->mutex,
                               ty_adjust_timeout(pool->unused_timeout, start));
        }

        pool->busy++;
        ty_mutex_unlock(&pool->mutex);

        run_task(task);
        ty_task_unref(task);
    }

timeout:
    pool->started--;
    if (pool->init) {
        ty_list_remove(&thread->list);
        ty_thread_detach(&thread->thread);
        free(thread);
    }
    ty_mutex_unlock(&pool->mutex);

    return 0;
}

// Call with pool->mutex locked
static int start_thread(ty_pool *pool)
{
    struct pool_thread *thread;
    int r;

    thread = calloc(1, sizeof(*thread));
    if (!thread) {
        r = ty_error(TY_ERROR_MEMORY, NULL);
        goto cleanup;
    }

    thread->pool = pool;

    r = ty_thread_create(&thread->thread, task_thread, thread);
    if (r < 0)
        goto cleanup;

    ty_list_add(&pool->threads, &thread->list);
    pool->started++;
    pool->busy++;

    return 0;

cleanup:
    free(thread);
    return r;
}

int ty_task_start(ty_task *task)
{
    assert(task);
    assert(task->status == TY_TASK_STATUS_READY);

    ty_pool *pool;
    int r;

    if (!task->pool) {
        r = ty_pool_get_default(&task->pool);
        if (r < 0)
            return r;
    }
    pool = task->pool;

    ty_mutex_lock(&pool->mutex);

    if (pool->busy == pool->started && pool->started < pool->max_threads) {
        r = start_thread(pool);
        if (r < 0)
            goto cleanup;
    }

    ty_task_ref(task);
    ty_list_add(&pool->pending_tasks, &task->list);
    ty_cond_signal(&pool->pending_cond);

    change_status(task, TY_TASK_STATUS_PENDING);

    r = 0;
cleanup:
    ty_mutex_unlock(&pool->mutex);
    return r;
}

int ty_task_wait(ty_task *task, ty_task_status status, int timeout)
{
    assert(task);
    assert(status > TY_TASK_STATUS_READY);

    uint64_t start;
    int r;

    /* If the caller wants to wait until the task has finished without timing out, try
       to execute the task in this thread if it's not running already. */
    if (status == TY_TASK_STATUS_FINISHED && timeout < 0) {
        if (task->status == TY_TASK_STATUS_PENDING) {
            ty_mutex_lock(&task->pool->mutex);
            if (task->status == TY_TASK_STATUS_PENDING) {
                ty_list_remove(&task->list);
                ty_task_unref(task);

                task->status = TY_TASK_STATUS_READY;
            }
            ty_mutex_unlock(&task->pool->mutex);
        }

        if (task->status == TY_TASK_STATUS_READY) {
            run_task(task);
            return 1;
        }
    } else if (task->status == TY_TASK_STATUS_READY) {
        r = ty_task_start(task);
        if (r < 0)
            return r;
    }

    ty_mutex_lock(&task->mutex);
    start = ty_millis();
    while (task->status < status) {
        if (!ty_cond_wait(&task->cond, &task->mutex, ty_adjust_timeout(timeout, start)))
            break;
    }
    r = task->status >= status;
    ty_mutex_unlock(&task->mutex);

    return r;
}

int ty_task_join(ty_task *task)
{
    assert(task);

    int r = ty_task_wait(task, TY_TASK_STATUS_FINISHED, -1);
    if (r < 0)
        return r;

    return task->ret;
}

ty_task *ty_task_get_current(void)
{
    return current_task;
}
