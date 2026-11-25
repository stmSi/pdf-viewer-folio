#ifndef FOLIO_THREAD_H
#define FOLIO_THREAD_H

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HANDLE FolioThread;
typedef CRITICAL_SECTION FolioMutex;
typedef CONDITION_VARIABLE FolioCondition;
static inline void mutex_init(FolioMutex *m) { InitializeCriticalSection(m); }
static inline void mutex_lock(FolioMutex *m) { EnterCriticalSection(m); }
static inline void mutex_unlock(FolioMutex *m) { LeaveCriticalSection(m); }
static inline void mutex_destroy(FolioMutex *m) { DeleteCriticalSection(m); }
static inline void condition_init(FolioCondition *c) { InitializeConditionVariable(c); }
static inline void condition_wait(FolioCondition *c, FolioMutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
static inline void condition_signal(FolioCondition *c) { WakeConditionVariable(c); }
static inline void condition_destroy(FolioCondition *c) { (void)c; }
#define THREAD_RETURN DWORD WINAPI
#define THREAD_DONE 0
static inline int thread_start(FolioThread *t, LPTHREAD_START_ROUTINE fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t == NULL ? -1 : 0;
}
static inline void thread_detach(FolioThread t) { CloseHandle(t); }
#else
#include <pthread.h>
typedef pthread_t FolioThread;
typedef pthread_mutex_t FolioMutex;
typedef pthread_cond_t FolioCondition;
static inline void mutex_init(FolioMutex *m) { pthread_mutex_init(m, NULL); }
static inline void mutex_lock(FolioMutex *m) { pthread_mutex_lock(m); }
static inline void mutex_unlock(FolioMutex *m) { pthread_mutex_unlock(m); }
static inline void mutex_destroy(FolioMutex *m) { pthread_mutex_destroy(m); }
static inline void condition_init(FolioCondition *c) { pthread_cond_init(c, NULL); }
static inline void condition_wait(FolioCondition *c, FolioMutex *m) { pthread_cond_wait(c, m); }
static inline void condition_signal(FolioCondition *c) { pthread_cond_signal(c); }
static inline void condition_destroy(FolioCondition *c) { pthread_cond_destroy(c); }
#define THREAD_RETURN void *
#define THREAD_DONE NULL
static inline int thread_start(FolioThread *t, void *(*fn)(void *), void *arg) { return pthread_create(t, NULL, fn, arg); }
static inline void thread_detach(FolioThread t) { pthread_detach(t); }
#endif
#endif
