#define _GNU_SOURCE
#include "mythread.h"

#include <linux/futex.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)
#define THREAD_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM)

enum { ST_DETACHED = 1, ST_JOINED = 2, ST_CANCELED = 4 };

struct mythread {
    int tid;
    int state;
    void *(*start)(void *);
    void *arg;
    void *retval;
    size_t map_size;
    struct mythread *next;
};

static struct mythread *threads;
static char list_lock;
static int reaper_started;
static _Alignas(16) char reaper_stack[64 * 1024];

static void lock_list(void)
{
    while (__atomic_test_and_set(&list_lock, __ATOMIC_ACQUIRE))
        sched_yield();
}

static void unlock_list(void)
{
    __atomic_clear(&list_lock, __ATOMIC_RELEASE);
}

// искать только под блокировкой
static struct mythread **find_thread(mythread_t thread)
{
    struct mythread **link = &threads;
    while (*link && *link != thread)
        link = &(*link)->next;
    return link;
}

static int release_thread(struct mythread **link)
{
    struct mythread *t = *link;
    struct mythread *next = t->next;
    if (munmap(t, t->map_size) == -1)
        return errno;
    *link = next;
    return 0;
}

void mythread_reap(void)
{
    lock_list();
    struct mythread **link = &threads;
    while (*link) {
        struct mythread *t = *link;
        if ((t->state & ST_DETACHED) &&
            __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE) == 0 &&
            release_thread(link) == 0)
            continue;
        link = &t->next;
    }
    unlock_list();
}

static int reaper_entry(void *arg)
{
    (void)arg; //убираем предупреждение об неиспользуемом параметре
    for (;;) {
        mythread_reap();
        struct timespec delay = {0, 10000000};//10мс
        syscall(SYS_nanosleep, &delay, NULL);
    }
    return 0;
}

// вызывается под блокировкой
static int start_reaper(void)
{
    if (reaper_started)
        return 0;
    if (clone(reaper_entry, reaper_stack + sizeof(reaper_stack),
              THREAD_FLAGS, NULL) == -1)
        return errno;
    reaper_started = 1;
    return 0;
}

static int thread_entry(void *arg)
{
    struct mythread *t = arg;
    t->retval = t->start(t->arg);
    return 0;
}

int mythread_create(mythread_t *thread, void *(*start_routine)(void *), void *arg)
{
    if (!thread || !start_routine)
        return EINVAL;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (size_t)page < sizeof(struct mythread))
        return EINVAL;

    // дескриптор, защитная страница и стек
    size_t size = STACK_SIZE + 2 * (size_t)page;
    struct mythread *t = mmap(NULL, size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (t == MAP_FAILED)
        return errno;
    if (mprotect((char *)t + page, (size_t)page, PROT_NONE) == -1) {
        int error = errno;
        munmap(t, size);
        return error;
    }

    t->start = start_routine;
    t->arg = arg;
    t->map_size = size;

    int flags = THREAD_FLAGS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
    if (clone(thread_entry, (char *)t + size, flags, t,
              &t->tid, NULL, &t->tid) == -1) {
        int error = errno;
        munmap(t, size);
        return error;
    }

    lock_list();
    t->next = threads;
    threads = t;
    unlock_list();
    *thread = t;
    return 0;
}

int mythread_join(mythread_t t, void **retval)
{
    lock_list();
    if (!t || !*find_thread(t)) {
        unlock_list();
        return EINVAL;
    }
    if (__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE) == (int)syscall(SYS_gettid)) {//самого себя не джоиним
        unlock_list();
        return EDEADLK;
    }
    if (t->state & (ST_DETACHED | ST_JOINED)) {
        unlock_list();
        return EINVAL;
    }
    t->state |= ST_JOINED;
    unlock_list();

    int tid;
    while ((tid = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE)) != 0) {
        // ядро будит общий futex
        if (syscall(SYS_futex, &t->tid, FUTEX_WAIT, tid, NULL, NULL, 0) == -1) {//усыпляет поток, если текущее значение РАВНО ожидаемому
            //join не удался
            int error = errno;
             // EAGAIN: значение уже изменилось
            if (error != EAGAIN && error != EINTR) { 
                lock_list();
                t->state &= ~ST_JOINED;//сбрасываем флажок чтобы другой поток не попытался одновременно выполнить join для того же дескриптора
                unlock_list();
                return error;
            }
        }
    }

    lock_list();
    void *result = t->retval;
    int error = release_thread(find_thread(t));
    if (error)
        t->state &= ~ST_JOINED;
    unlock_list();
    if (!error && retval)
        *retval = result;
    return error;
}

int mythread_detach(mythread_t t)
{
    lock_list();
    if (!t || !*find_thread(t) || (t->state & (ST_DETACHED | ST_JOINED))) {
        unlock_list();
        return EINVAL;
    }
    int error = start_reaper();
    if (!error)
        t->state |= ST_DETACHED;
    unlock_list();
    return error;
}

int mythread_cancel(mythread_t t)
{
    lock_list();
    if (!t || !*find_thread(t) || (t->state & ST_DETACHED)) {
        unlock_list();
        return EINVAL;
    }
    if (__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE) == 0) {
        unlock_list();
        return ESRCH;
    }
    // отмена разрешена и во время join
    t->state |= ST_CANCELED;
    unlock_list();
    return 0;
}

int mythread_testcancel(mythread_t t)
{
    lock_list();
    if (!t || !*find_thread(t)) {
        unlock_list();
        return EINVAL;
    }
    if (__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE) != (int)syscall(SYS_gettid)) {
        unlock_list();
        return EPERM;
    }
    int canceled = t->state & ST_CANCELED;
    unlock_list();
    if (!canceled)
        return 0;

    t->retval = MYTHREAD_CANCELED;
    syscall(SYS_exit, 0);
}
