#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * posix_ipc_helpers.c
 * ===================
 * Generic helper utilities for OS labs focused on:
 * - mmap (anonymous and file/shm-backed)
 * - POSIX shared memory objects (shm_open/ftruncate/mmap/shm_unlink)
 * - POSIX named semaphores (sem_open/sem_wait/sem_post/sem_unlink)
 * - process-shared barrier/mutex/condvar
 * - robust mutex crash recovery (EOWNERDEAD)
 *
 * This file intentionally avoids any domain story.
 *
 * Typical compile line:
 *   gcc -std=c11 -Wall -Wextra -O2 your_program.c posix_ipc_helpers.c -pthread -lrt
 */

#define IPCH_DIE(msg)                                                          \
    do {                                                                       \
        int _e = errno;                                                        \
        fprintf(stderr, "[%s:%d] %s failed: errno=%d (%s)\n", __FILE__,       \
                __LINE__, (msg), _e, strerror(_e));                           \
        kill(0, SIGKILL);                                                      \
        exit(EXIT_FAILURE);                                                    \
    } while (0)

/*  Small utility functions  */

void ipch_msleep(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;

    while (nanosleep(&ts, &ts) == -1) {
        if (errno == EINTR) {
            continue;
        }
        IPCH_DIE("nanosleep");
    }
}

void ipch_seed_rng(void) {
    unsigned int seed = (unsigned int)(time(NULL) ^ getpid() ^ (uintptr_t)&seed);
    srand(seed);
}

int ipch_rand_inclusive(int lo, int hi) {
    if (lo > hi) {
        fprintf(stderr, "ipch_rand_inclusive invalid range: [%d,%d]\n", lo, hi);
        exit(EXIT_FAILURE);
    }
    return lo + (rand() % (hi - lo + 1));
}

size_t ipch_flat_index(size_t row, size_t col, size_t cols_per_row) {
    return row * cols_per_row + col;
}

/* -------------------- Named semaphores (POSIX API wrapper) ----------------- */

/*
 * Naming convention:
 * - Named semaphores must begin with '/' on Linux, e.g. "/lab-sem-0".
 */
void ipch_make_sem_name(char *dst, size_t dst_size, const char *prefix, int idx) {
    int n = snprintf(dst, dst_size, "%s%d", prefix, idx);
    if (n < 0 || (size_t)n >= dst_size) {
        errno = ENAMETOOLONG;
        IPCH_DIE("snprintf semaphore name");
    }
}

/*
 * sem_open(name, O_CREAT, mode, value)
 * - O_CREAT creates if missing.
 * - mode controls permissions at creation.
 * - value is only used on creation.
 */
sem_t *ipch_sem_open_named(const char *name, unsigned initial_value) {
    sem_t *s = sem_open(name, O_CREAT, 0666, initial_value);
    if (s == SEM_FAILED) {
        IPCH_DIE("sem_open");
    }
    return s;
}

/*
 * sem_unlink removes the name from namespace.
 * Existing opened instances remain valid until sem_close by all users.
 */
void ipch_sem_unlink_if_exists(const char *name) {
    if (sem_unlink(name) == -1 && errno != ENOENT) {
        IPCH_DIE("sem_unlink");
    }
}

void ipch_sem_wait_intr(sem_t *sem) {
    while (sem_wait(sem) == -1) {
        if (errno == EINTR) {
            continue;
        }
        IPCH_DIE("sem_wait");
    }
}

void ipch_sem_post_checked(sem_t *sem) {
    if (sem_post(sem) == -1) {
        IPCH_DIE("sem_post");
    }
}

void ipch_sem_close_checked(sem_t *sem) {
    if (sem_close(sem) == -1) {
        IPCH_DIE("sem_close");
    }
}

/*
 * Batch helpers useful in many tasks:
 * Create/open N semaphores named "<prefix><i>".
 */
void ipch_open_sem_array(sem_t **arr, int n, const char *prefix, unsigned initial_value) {
    char name[128];
    for (int i = 0; i < n; ++i) {
        ipch_make_sem_name(name, sizeof(name), prefix, i);
        arr[i] = ipch_sem_open_named(name, initial_value);
    }
}

void ipch_close_sem_array(sem_t **arr, int n) {
    for (int i = 0; i < n; ++i) {
        if (arr[i]) {
            ipch_sem_close_checked(arr[i]);
            arr[i] = NULL;
        }
    }
}

void ipch_unlink_sem_array_if_exists(int n, const char *prefix) {
    char name[128];
    for (int i = 0; i < n; ++i) {
        ipch_make_sem_name(name, sizeof(name), prefix, i);
        ipch_sem_unlink_if_exists(name);
    }
}

/* --------------- Anonymous shared memory via mmap (MAP_ANON) --------------- */

/*
 * Shared between parent/child after fork:
 * - MAP_SHARED + MAP_ANONYMOUS creates unnamed shared mapping.
 * - Good for control metadata (barriers/mutexes/flags).
 */
void *ipch_mmap_anon_shared(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        IPCH_DIE("mmap anonymous shared");
    }
    memset(p, 0, bytes);
    return p;
}

void ipch_munmap_checked(void *addr, size_t bytes) {
    if (munmap(addr, bytes) == -1) {
        IPCH_DIE("munmap");
    }
}

/* ---------------- Named shared memory (shm_open + mmap) wrappers ----------- */

/*
 * shm_open namespace also expects names beginning with '/', e.g. "/lab-shm".
 */
int ipch_shm_open_create_rw(const char *name, mode_t mode) {
    int fd = shm_open(name, O_CREAT | O_RDWR, mode);
    if (fd == -1) {
        IPCH_DIE("shm_open O_CREAT|O_RDWR");
    }
    return fd;
}

int ipch_shm_open_rw(const char *name) {
    int fd = shm_open(name, O_RDWR, 0);
    if (fd == -1) {
        IPCH_DIE("shm_open O_RDWR");
    }
    return fd;
}

void ipch_ftruncate_checked(int fd, size_t bytes) {
    if (ftruncate(fd, (off_t)bytes) == -1) {
        IPCH_DIE("ftruncate");
    }
}

void *ipch_mmap_shared_rw(int fd, size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        IPCH_DIE("mmap MAP_SHARED");
    }
    return p;
}

void ipch_close_fd_checked(int fd) {
    if (close(fd) == -1) {
        IPCH_DIE("close");
    }
}

void ipch_shm_unlink_if_exists(const char *name) {
    if (shm_unlink(name) == -1 && errno != ENOENT) {
        IPCH_DIE("shm_unlink");
    }
}

/*
 * One-stop helper for creator process:
 * - create/open named shm
 * - set size
 * - map writable shared region
 * Returns mapped pointer and outputs fd via out_fd.
 */
void *ipch_create_map_shm_rw(const char *name, size_t bytes, int *out_fd) {
    int fd = ipch_shm_open_create_rw(name, 0666);
    ipch_ftruncate_checked(fd, bytes);
    void *p = ipch_mmap_shared_rw(fd, bytes);
    if (out_fd) {
        *out_fd = fd;
    }
    return p;
}

/* ---------------- Process-shared pthread primitive initializers ------------ */

/*
 * Barrier visible across processes:
 * - initialize in shared memory.
 * - count should include all participants (e.g., children + parent).
 */
void ipch_barrier_init_pshared(pthread_barrier_t *barrier, unsigned count) {
    pthread_barrierattr_t attr;
    int rc = pthread_barrierattr_init(&attr);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_barrierattr_init");
    }
    rc = pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_barrierattr_setpshared");
    }
    rc = pthread_barrier_init(barrier, &attr, count);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_barrier_init");
    }
    rc = pthread_barrierattr_destroy(&attr);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_barrierattr_destroy");
    }
}

void ipch_barrier_wait_checked(pthread_barrier_t *barrier) {
    int rc = pthread_barrier_wait(barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        errno = rc;
        IPCH_DIE("pthread_barrier_wait");
    }
}

void ipch_barrier_destroy_checked(pthread_barrier_t *barrier) {
    int rc = pthread_barrier_destroy(barrier);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_barrier_destroy");
    }
}

/*
 * process-shared mutex attributes.
 * robust=true enables EOWNERDEAD handling for crash-recovery patterns.
 */
void ipch_mutexattr_init_pshared(pthread_mutexattr_t *attr, bool robust) {
    int rc = pthread_mutexattr_init(attr);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_mutexattr_init");
    }
    rc = pthread_mutexattr_setpshared(attr, PTHREAD_PROCESS_SHARED);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_mutexattr_setpshared");
    }
    if (robust) {
        rc = pthread_mutexattr_setrobust(attr, PTHREAD_MUTEX_ROBUST);
        if (rc != 0) {
            errno = rc;
            IPCH_DIE("pthread_mutexattr_setrobust");
        }
    }
}

void ipch_mutex_init_checked(pthread_mutex_t *m, const pthread_mutexattr_t *attr) {
    int rc = pthread_mutex_init(m, attr);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_mutex_init");
    }
}

void ipch_mutex_destroy_checked(pthread_mutex_t *m) {
    int rc = pthread_mutex_destroy(m);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_mutex_destroy");
    }
}

void ipch_mutex_lock_checked(pthread_mutex_t *m) {
    int rc = pthread_mutex_lock(m);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_mutex_lock");
    }
}

void ipch_mutex_unlock_checked(pthread_mutex_t *m) {
    int rc = pthread_mutex_unlock(m);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_mutex_unlock");
    }
}

/*
 * Robust-mutex lock wrapper:
 * returns:
 *   0 -> locked normally
 *   1 -> owner died, now made consistent and locked by caller
 * Exits on other errors.
 */
int ipch_mutex_lock_robust_recover(pthread_mutex_t *m) {
    int rc = pthread_mutex_lock(m);
    if (rc == 0) {
        return 0;
    }
    if (rc == EOWNERDEAD) {
        rc = pthread_mutex_consistent(m);
        if (rc != 0) {
            errno = rc;
            IPCH_DIE("pthread_mutex_consistent");
        }
        return 1;
    }
    errno = rc;
    IPCH_DIE("pthread_mutex_lock robust");
    return -1;
}

/* Optional condition-variable helpers for producer/consumer style labs. */
void ipch_condattr_init_pshared(pthread_condattr_t *attr) {
    int rc = pthread_condattr_init(attr);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_condattr_init");
    }
    rc = pthread_condattr_setpshared(attr, PTHREAD_PROCESS_SHARED);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_condattr_setpshared");
    }
}

void ipch_cond_init_checked(pthread_cond_t *cv, const pthread_condattr_t *attr) {
    int rc = pthread_cond_init(cv, attr);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_cond_init");
    }
}

void ipch_cond_wait_checked(pthread_cond_t *cv, pthread_mutex_t *m) {
    int rc = pthread_cond_wait(cv, m);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_cond_wait");
    }
}

void ipch_cond_signal_checked(pthread_cond_t *cv) {
    int rc = pthread_cond_signal(cv);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_cond_signal");
    }
}

void ipch_cond_broadcast_checked(pthread_cond_t *cv) {
    int rc = pthread_cond_broadcast(cv);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_cond_broadcast");
    }
}

void ipch_cond_destroy_checked(pthread_cond_t *cv) {
    int rc = pthread_cond_destroy(cv);
    if (rc != 0) {
        errno = rc;
        IPCH_DIE("pthread_cond_destroy");
    }
}

/*  Process orchestration  */

void ipch_spawn_children(int n, void (*child_fn)(void *), void *arg) {
    for (int i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == -1) {
            IPCH_DIE("fork");
        }
        if (pid == 0) {
            child_fn(arg);
            _exit(EXIT_SUCCESS);
        }
    }
}

void ipch_wait_all_children(void) {
    while (wait(NULL) > 0) {
    }
    if (errno != ECHILD) {
        IPCH_DIE("wait");
    }
}

/*  Reference usage snippets */

/*
 * PATTERN A: anonymous control block
 *
 * typedef struct {
 *     pthread_barrier_t b;
 *     pthread_mutex_t m;
 *     int flag;
 * } shared_ctrl_t;
 *
 * shared_ctrl_t *ctrl = ipch_mmap_anon_shared(sizeof(*ctrl));
 * ipch_barrier_init_pshared(&ctrl->b, n_children + 1);
 * pthread_mutexattr_t ma;
 * ipch_mutexattr_init_pshared(&ma, true);
 * ipch_mutex_init_checked(&ctrl->m, &ma);
 * pthread_mutexattr_destroy(&ma);
 */

/*
 * PATTERN B: named shared array
 *
 * size_t count = m * k;
 * size_t bytes = count * sizeof(double);
 * int shm_fd;
 * double *arr = ipch_create_map_shm_rw("/lab-shm", bytes, &shm_fd);
 * for (size_t i = 0; i < count; ++i) arr[i] = 1.0;
 *
 * // child side:
 * int cfd = ipch_shm_open_rw("/lab-shm");
 * double *carr = ipch_mmap_shared_rw(cfd, bytes);
 */

/*
 * PATTERN C: indexed semaphore set
 *
 * sem_t *sems[MAX_ITEMS];
 * ipch_unlink_sem_array_if_exists(item_count, "/lab-sem-");
 * ipch_open_sem_array(sems, item_count, "/lab-sem-", CAP);
 * ...
 * ipch_sem_wait_intr(sems[idx]);
 * // critical section
 * ipch_sem_post_checked(sems[idx]);
 * ...
 * ipch_close_sem_array(sems, item_count);
 * ipch_unlink_sem_array_if_exists(item_count, "/lab-sem-");
 */
