#define _GNU_SOURCE
#include "mythread.h"

#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>
#include <unistd.h>

struct cancel_data {
    mythread_t thread;
    atomic_int ready; // дескриптор готов
};

static void *worker(void *arg)
{
    return arg;
}

static void *detached_worker(void *arg)
{
    atomic_int *done = arg;
    atomic_store_explicit(done, 1, memory_order_release);
    return NULL;
}

static void *cancel_worker(void *arg) 
{
    struct cancel_data *data = arg;
    while (!atomic_load_explicit(&data->ready, memory_order_acquire))
        sched_yield();

    for (;;) {
        if (mythread_testcancel(data->thread) != 0)
            return NULL;
        sched_yield();
    }
    return NULL;
}

int main(void)
{
    mythread_t thread;
    void *result;
    int error = mythread_create(&thread, worker, "обычный поток");
    if (error != 0) {
        fprintf(stderr, "mythread_create: %d\n", error);
        return 1;
    }
    error = mythread_join(thread, &result);
    if (error != 0) {
        fprintf(stderr, "mythread_join: %d\n", error);
        return 1;
    }
    printf("join вернул: %s\n", (const char *)result);

    atomic_int done = 0;
    error = mythread_create(&thread, detached_worker, &done);
    if (error == 0)
        error = mythread_detach(thread);
    if (error != 0) {
        fprintf(stderr, "не удалось создать или отсоединить поток\n");
        return 1;
    }

    while (!atomic_load_explicit(&done, memory_order_acquire))
        sched_yield();
    puts("detached поток выполнил работу");

    struct cancel_data data = {0};
    error = mythread_create(&thread, cancel_worker, &data);
    if (error != 0) {
        fprintf(stderr, "mythread_create: %d\n", error);
        return 1;
    }
    data.thread = thread;
    atomic_store_explicit(&data.ready, 1, memory_order_release);
    error = mythread_cancel(thread);
    if (error != 0) {
        fprintf(stderr, "mythread_cancel: %d\n", error);
        return 1;
    }
    error = mythread_join(thread, &result);
    if (error != 0) {
        fprintf(stderr, "mythread_join: %d\n", error);
        return 1;
    }
    printf("поток отменён: %s\n", result == MYTHREAD_CANCELED ? "да" : "нет");

    return result == MYTHREAD_CANCELED ? 0 : 1;
}
