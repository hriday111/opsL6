#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>

#define KEYBOARD_CAP 10
#define SHARED_MEM_NAME "/memory"
#define MIN_STUDENTS KEYBOARD_CAP
#define MAX_STUDENTS 20
#define MIN_KEYBOARDS 1
#define MAX_KEYBOARDS 5
#define MIN_KEYS 5
#define MAX_KEYS KEYBOARD_CAP

#define ERR(source)                                     \
    do                                                  \
    {                                                   \
        fprintf(stderr, "%s:%d\n", __FILE__, __LINE__); \
        perror(source);                                 \
        kill(0, SIGKILL);                               \
        exit(EXIT_FAILURE);                             \
    } while (0)


typedef struct shared{
    pthread_barrier_t barrier;
    pthread_mutex_t keyboards[];
} shared_t;
void usage(char* program_name)
{
    fprintf(stderr, "Usage: \n");
    fprintf(stderr, "\t%s n m k\n", program_name);
    fprintf(stderr, "\t  n - number of students, %d <= n <= %d\n", MIN_STUDENTS, MAX_STUDENTS);
    fprintf(stderr, "\t  m - number of keyboards, %d <= m <= %d\n", MIN_KEYBOARDS, MAX_KEYBOARDS);
    fprintf(stderr, "\t  k - number of keys in a keyboard, %d <= k <= %d\n", MIN_KEYS, MAX_KEYS);
    exit(EXIT_FAILURE);
}

void ms_sleep(unsigned int milli)
{
    time_t sec = (int)(milli / 1000);
    milli = milli - (sec * 1000);
    struct timespec ts = {0};
    ts.tv_sec = sec;
    ts.tv_nsec = milli * 1000000L;
    if (nanosleep(&ts, &ts))
        ERR("nanosleep");
}

void print_keyboards_state(double* keyboards, int m, int k)
{
    for (int i=0;i<m;++i)
    {
        printf("Klawiatura nr %d:\n", i + 1);
        for (int j=0;j<k;++j)
            printf("  %e", keyboards[i * k + j]);
        printf("\n\n");
    }
}

void child_work(int m, int k, shared_t *shared)
{
    pthread_barrier_wait(&shared->barrier);
    char sem_name[20];
    sem_t *sem_arr[MAX_KEYBOARDS];
    srand(time(NULL)^getpid());
    for(int i=0; i<m;i++)
    {
        snprintf(sem_name, sizeof(sem_name), "/sop-sem-%d", i);
        sem_arr[i] = sem_open(sem_name, O_CREAT, 0666, KEYBOARD_CAP);
        if(sem_arr[i]==SEM_FAILED) {ERR("sem_open");}
    }

    int fd = shm_open(SHARED_MEM_NAME,  O_CREAT | O_RDWR, 0666);
    if(fd=-1){ERR("shm_open");}

    double* shared_mem = mmap(NULL, m*k*sizeof(double), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(shared_mem=MAP_FAILED) {ERR("mmap");}

    for(int i=0; i<10; i++)
    {
        int rand_sem=rand()%m;
        int rand_val = rand()%(m*k);
        sem_wait(sem_arr[rand_val%m]);
        printf("Student %d: cleaning keyboard %d\n", getpid(), rand_sem);
        ms_sleep(300);
        pthread_mutex_lock(&shared->keyboards[rand_sem]);
        shared_mem[rand_sem]/=3;

        pthread_mutex_lock(&shared->keyboards[rand_sem]);
        sem_post(sem_arr[rand_sem%m]);
    }

    for(int i = 0; i < m; i++){
        if(sem_close(sem_arr[i]) == -1){
            ERR("sem_close");
        }
    }
    
    munmap(shared, sizeof(shared_t));

    munmap(shared_mem, m*k*sizeof(double));
    close(fd);
}


void clean_sems(int m)
{
    char sem_name[20];
    for(int i=0; i<m; i++)
    {
        snprintf(sem_name, sizeof(sem_name), "/sop-sem-%d", i);
        sem_unlink(sem_name);
    }
}
void create_n_processes(int n, int m, int k, shared_t* shared)
{
    for(int i=0; i<n; i++)
    {
        pid_t pid = fork();
        if(pid<0)
        {
            ERR("fork");
        }
        if(pid==0)
        {

            child_work(m,k, shared);
            exit(EXIT_SUCCESS);
        }
    }
}
int main(int argc, char** argv) { 
    if(argc!=4) {usage(argv[0]);}
    int n= atoi(argv[1]);
    int m= atoi(argv[2]);
    int k= atoi(argv[3]);

    if(n<KEYBOARD_CAP || n>20 ||m<1 ||m>5 || k<5 || k>KEYBOARD_CAP)
    {
        usage(argv[0]);
    }
    clean_sems(m);
    shared_t* shared = mmap(NULL, sizeof(shared_t), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANON,-1,0);
    if(shared==MAP_FAILED)
    {
        ERR("mmap");
    }
    pthread_barrierattr_t barrier_attr;
    pthread_barrierattr_init(&barrier_attr);
    pthread_barrierattr_setpshared(&barrier_attr, PTHREAD_PROCESS_SHARED);
    pthread_barrier_init(&shared->barrier, &barrier_attr, n+1);
    pthread_barrierattr_destroy(&barrier_attr);

    pthread_mutexattr_t mutex_attr;
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    for(int i=0; i<m*k; i++)
    {
        pthread_mutex_init(&shared->keyboards[i], &mutex_attr);
    }
    pthread_mutexattr_destroy(&mutex_attr);
    
    create_n_processes(n,m,k, shared);

    int fd = shm_open(SHARED_MEM_NAME,  O_CREAT | O_RDWR, 0666);
    if(fd=-1){ERR("shm_open");}

    if(ftruncate(fd, m*k*sizeof(double))==-1)
    {
        ERR("truncate");
    }

    double* shared_mem = mmap(NULL, m*k*sizeof(double), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(shared_mem=MAP_FAILED) {ERR("mmap");}

    for(int i=0; i<k*m;i++)
    {
        shared_mem[i]=1.0;
    }

    ms_sleep(500);
    pthread_barrier_wait(&shared->barrier);
    while(wait(NULL)>0){}
    clean_sems(m);
    close(fd);
    pthread_barrier_destroy(&shared->barrier);
    munmap(shared, sizeof(shared_t));
    munmap(shared_mem, m*k*sizeof(double));
    printf("Cleaning Finished\n");
    return EXIT_SUCCESS;
}