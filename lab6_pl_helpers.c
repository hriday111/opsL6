#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * LAB 6 helper toolbox
 * --------------------
 * This file is intended as a "bring from home" utility file for tasks that
 * look similar to:
 * - multiple processes (fork)
 * - POSIX named semaphores
 * - anonymous and named shared memory
 * - process-shared barriers and mutexes
 * - robust mutex handling for crash detection
 *
 * Copy selected functions into tomorrow's exam file if required, or compile
 * this file together with your solution and call these helpers directly.
 */

#define LAB6_MAX_KEYBOARDS 5
#define LAB6_MAX_KEYS_PER_KEYBOARD 10
#define LAB6_SEM_PREFIX "/sop-sem-"
#define LAB6_SHARED_MEM_NAME "/memory"

#define DIE(msg)                                                               \
    do {                                                                       \
        int _err = errno;                                                      \
        fprintf(stderr, "[%s:%d] %s failed (errno=%d: %s)\n", __FILE__,       \
                __LINE__, (msg), _err, strerror(_err));                       \
        kill(0, SIGKILL);                                                      \
        exit(EXIT_FAILURE);                                                    \
    } while (0)

typedef struct stage24_shared {
    pthread_barrier_t startup_barrier; /* children + parent synchronization */
    pthread_mutex_t key_mutex[LAB6_MAX_KEYBOARDS * LAB6_MAX_KEYS_PER_KEYBOARD];
    pthread_mutex_t panic_mutex;
    int panic_flag; /* 0 = continue, 1 = panic stop requested */
} stage24_shared_t;

static inline int key_index(int keyboard_idx, int key_idx, int keys_per_keyboard) {
    return keyboard_idx * keys_per_keyboard + key_idx;
}

void ms_sleep(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1) {
        if (errno == EINTR) {
            continue;
        }
        DIE("nanosleep");
    }
}

void rng_seed_per_process(void) {
    unsigned int seed = (unsigned int)(time(NULL) ^ getpid() ^ (uintptr_t)&seed);
    srand(seed);
}

int rand_range(int low, int high_inclusive) {
    if (low > high_inclusive) {
        fprintf(stderr, "rand_range: invalid bounds [%d, %d]\n", low, high_inclusive);
        exit(EXIT_FAILURE);
    }
    int span = high_inclusive - low + 1;
    return low + (rand() % span);
}

void print_keyboards_state(const double *keys, int keyboard_count, int keys_per_keyboard) {
    for (int kb = 0; kb < keyboard_count; ++kb) {
        printf("Keyboard %d:\n", kb);
        for (int key = 0; key < keys_per_keyboard; ++key) {
            printf("  %.6e", keys[key_index(kb, key, keys_per_keyboard)]);
        }
        printf("\n\n");
    }
}

/* -------------------- Stage 1 helpers: named semaphores ------------------- */

void sem_name_at(char *dst, size_t dst_size, int keyboard_idx) {
    int written = snprintf(dst, dst_size, LAB6_SEM_PREFIX "%d", keyboard_idx);
    if (written < 0 || (size_t)written >= dst_size) {
        errno = ENAMETOOLONG;
        DIE("snprintf semaphore name");
    }
}

void cleanup_keyboard_semaphores_if_exist(int keyboard_count) {
    char name[64];
    for (int i = 0; i < keyboard_count; ++i) {
        sem_name_at(name, sizeof(name), i);
        if (sem_unlink(name) == -1 && errno != ENOENT) {
            DIE("sem_unlink");
        }
    }
}

void open_keyboard_semaphores(sem_t **sem_arr, int keyboard_count, int keyboard_cap) {
    char name[64];
    for (int i = 0; i < keyboard_count; ++i) {
        sem_name_at(name, sizeof(name), i);
        sem_arr[i] = sem_open(name, O_CREAT, 0666, keyboard_cap);
        if (sem_arr[i] == SEM_FAILED) {
            DIE("sem_open");
        }
    }
}

void close_keyboard_semaphores(sem_t **sem_arr, int keyboard_count) {
    for (int i = 0; i < keyboard_count; ++i) {
        if (sem_arr[i] == NULL) {
            continue;
        }
        if (sem_close(sem_arr[i]) == -1) {
            DIE("sem_close");
        }
        sem_arr[i] = NULL;
    }
}

/* -------- Stage 2 helpers: shared anonymous memory + barrier/mutex -------- */

stage24_shared_t *create_stage24_shared(int student_count, int keyboard_count, int keys_per_keyboard) {
    size_t map_size = sizeof(stage24_shared_t);
    stage24_shared_t *shared = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        DIE("mmap anonymous shared");
    }
    memset(shared, 0, map_size);

    pthread_barrierattr_t b_attr;
    if (pthread_barrierattr_init(&b_attr) != 0) {
        DIE("pthread_barrierattr_init");
    }
    if (pthread_barrierattr_setpshared(&b_attr, PTHREAD_PROCESS_SHARED) != 0) {
        DIE("pthread_barrierattr_setpshared");
    }
    if (pthread_barrier_init(&shared->startup_barrier, &b_attr, (unsigned)(student_count + 1)) != 0) {
        DIE("pthread_barrier_init");
    }
    if (pthread_barrierattr_destroy(&b_attr) != 0) {
        DIE("pthread_barrierattr_destroy");
    }

    pthread_mutexattr_t m_attr;
    if (pthread_mutexattr_init(&m_attr) != 0) {
        DIE("pthread_mutexattr_init");
    }
    if (pthread_mutexattr_setpshared(&m_attr, PTHREAD_PROCESS_SHARED) != 0) {
        DIE("pthread_mutexattr_setpshared");
    }
    if (pthread_mutexattr_setrobust(&m_attr, PTHREAD_MUTEX_ROBUST) != 0) {
        DIE("pthread_mutexattr_setrobust");
    }

    int total = keyboard_count * keys_per_keyboard;
    for (int i = 0; i < total; ++i) {
        if (pthread_mutex_init(&shared->key_mutex[i], &m_attr) != 0) {
            DIE("pthread_mutex_init key");
        }
    }
    if (pthread_mutex_init(&shared->panic_mutex, &m_attr) != 0) {
        DIE("pthread_mutex_init panic");
    }
    if (pthread_mutexattr_destroy(&m_attr) != 0) {
        DIE("pthread_mutexattr_destroy");
    }
    shared->panic_flag = 0;
    return shared;
}

void destroy_stage24_shared(stage24_shared_t *shared, int keyboard_count, int keys_per_keyboard) {
    int total = keyboard_count * keys_per_keyboard;
    for (int i = 0; i < total; ++i) {
        pthread_mutex_destroy(&shared->key_mutex[i]);
    }
    pthread_mutex_destroy(&shared->panic_mutex);
    pthread_barrier_destroy(&shared->startup_barrier);
    if (munmap(shared, sizeof(stage24_shared_t)) == -1) {
        DIE("munmap stage24 shared");
    }
}

/* ------- Stage 3 helpers: named shared memory object for keyboard data ---- */

int create_and_size_shared_keyboard_object(const char *name, int keyboard_count, int keys_per_keyboard) {
    size_t bytes = (size_t)keyboard_count * (size_t)keys_per_keyboard * sizeof(double);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        DIE("shm_open create");
    }
    if (ftruncate(fd, (off_t)bytes) == -1) {
        DIE("ftruncate shared keyboard object");
    }
    return fd;
}

int open_shared_keyboard_object(const char *name) {
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd == -1) {
        DIE("shm_open open");
    }
    return fd;
}

double *map_keyboard_state_rw(int fd, int keyboard_count, int keys_per_keyboard) {
    size_t bytes = (size_t)keyboard_count * (size_t)keys_per_keyboard * sizeof(double);
    double *arr = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (arr == MAP_FAILED) {
        DIE("mmap keyboard state");
    }
    return arr;
}

void init_keyboard_state(double *arr, int keyboard_count, int keys_per_keyboard, double value) {
    int total = keyboard_count * keys_per_keyboard;
    for (int i = 0; i < total; ++i) {
        arr[i] = value;
    }
}

void unmap_keyboard_state(double *arr, int keyboard_count, int keys_per_keyboard) {
    size_t bytes = (size_t)keyboard_count * (size_t)keys_per_keyboard * sizeof(double);
    if (munmap(arr, bytes) == -1) {
        DIE("munmap keyboard state");
    }
}

void close_and_unlink_shared_keyboard_object(int fd, const char *name) {
    if (close(fd) == -1) {
        DIE("close shm fd");
    }
    if (shm_unlink(name) == -1 && errno != ENOENT) {
        DIE("shm_unlink");
    }
}

/* ---------------- Stage 4 helpers: robust mutex + panic flag -------------- */

bool panic_requested(stage24_shared_t *shared) {
    if (pthread_mutex_lock(&shared->panic_mutex) != 0) {
        DIE("pthread_mutex_lock panic");
    }
    int flag = shared->panic_flag;
    if (pthread_mutex_unlock(&shared->panic_mutex) != 0) {
        DIE("pthread_mutex_unlock panic");
    }
    return flag != 0;
}

void request_panic(stage24_shared_t *shared) {
    if (pthread_mutex_lock(&shared->panic_mutex) != 0) {
        DIE("pthread_mutex_lock panic");
    }
    shared->panic_flag = 1;
    if (pthread_mutex_unlock(&shared->panic_mutex) != 0) {
        DIE("pthread_mutex_unlock panic");
    }
}

/*
 * Returns true if mutex is acquired normally.
 * Returns false if EOWNERDEAD occurred (caller should handle "injured student"
 * path and usually call request_panic()).
 */
bool lock_key_mutex_detect_owner_dead(stage24_shared_t *shared, int key_flat_idx) {
    int rc = pthread_mutex_lock(&shared->key_mutex[key_flat_idx]);
    if (rc == 0) {
        return true;
    }
    if (rc == EOWNERDEAD) {
        if (pthread_mutex_consistent(&shared->key_mutex[key_flat_idx]) != 0) {
            DIE("pthread_mutex_consistent");
        }
        return false;
    }
    errno = rc;
    DIE("pthread_mutex_lock key");
}

void unlock_key_mutex(stage24_shared_t *shared, int key_flat_idx) {
    int rc = pthread_mutex_unlock(&shared->key_mutex[key_flat_idx]);
    if (rc != 0) {
        errno = rc;
        DIE("pthread_mutex_unlock key");
    }
}

bool should_abort_with_one_percent(void) {
    return (rand() % 100) == 0;
}

/* -------------------------- Process orchestration -------------------------- */

void spawn_n_students(int n, void (*child_entry)(void *), void *arg) {
    for (int i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == -1) {
            DIE("fork");
        }
        if (pid == 0) {
            child_entry(arg);
            _exit(EXIT_SUCCESS);
        }
    }
}

void wait_for_all_children(void) {
    while (wait(NULL) > 0) {
    }
    if (errno != ECHILD) {
        DIE("wait");
    }
}

/*
 * Mini template for exam use:
 *
 * 1) Parse n,m,k and validate ranges.
 * 2) cleanup_keyboard_semaphores_if_exist(m);
 * 3) stage24_shared_t *shared = create_stage24_shared(n, m, k);
 * 4) spawn students (each:
 *      - wait on barrier
 *      - open semaphores
 *      - open/map shared keyboard array
 *      - do stage logic
 *      - close semaphores/unmap memory)
 * 5) parent creates+inits shared keyboard object:
 *      fd = create_and_size_shared_keyboard_object(LAB6_SHARED_MEM_NAME, m, k);
 *      arr = map_keyboard_state_rw(fd, m, k);
 *      init_keyboard_state(arr, m, k, 1.0);
 * 6) parent sleeps 500ms and releases barrier:
 *      ms_sleep(500);
 *      pthread_barrier_wait(&shared->startup_barrier);
 * 7) wait_for_all_children(), print_keyboards_state(), cleanup all.
 */
