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

void child_work(int m)
{
    char sem_name[20];
    sem_t *sem_arr[MAX_KEYBOARDS];
    srand(time(NULL)^getpid());
    for(int i=0; i<m;i++)
    {
        snprintf(sem_name, sizeof(sem_name), "/sop-sem-%d", i);
        sem_arr[i] = sem_open(sem_name, O_CREAT, 0666, KEYBOARD_CAP);
        if(sem_arr[i]==SEM_FAILED) {ERR("sem_open");}
    }
    for(int i=0; i<10; i++)
    {
        int rand_sem=rand()%m;
        sem_wait(sem_arr[rand_sem]);
        printf("Student %d: cleaning keyboard %d\n", getpid(), rand_sem);
        ms_sleep(300);
        sem_post(sem_arr[rand_sem]);
    }
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
void create_n_processes(int n, int m)
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

            child_work(m);
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
    create_n_processes(n,m);

    while(wait(NULL)>0){}
    clean_sems(m);
    printf("Cleaning Finished\n");
    return EXIT_SUCCESS;
}