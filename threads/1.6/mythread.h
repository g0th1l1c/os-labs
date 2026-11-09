#ifndef MYTHREAD_H
#define MYTHREAD_H

#include <errno.h>

// дескриптор потока
typedef struct mythread *mythread_t;

// результат отменённого потока
#define MYTHREAD_CANCELED ((void *)-1)

// создать поток и вернуть код ошибки
int mythread_create(mythread_t *thread,
                    void *(*start_routine)(void *),
                    void *arg);

// дождаться завершения и получить результат
int mythread_join(mythread_t thread, void **retval);

// отсоединить поток, ресурсы уберёт фоновый сборщик
int mythread_detach(mythread_t thread);

// запросить кооперативную отмену потока
int mythread_cancel(mythread_t thread);

// дополнительная уборка, вручную вызывать необязательно
void mythread_reap(void);

// точка отмены текущего потока, при отмене не возвращается
int mythread_testcancel(mythread_t thread);

#endif
