#define _GNU_SOURCE // 為了使用 sched_setaffinity 和 CPU_ZERO 等
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h> // 為了 clock_gettime

// 全域變數，用於同步
pthread_barrier_t barrier;

// 結構體：用於傳遞參數給每個執行緒
typedef struct {
    int id;
    int policy; // SCHED_OTHER or SCHED_FIFO
    int priority; // 優先級 (FIFO 才有用)
    double time_wait;
} thread_info_t;

/**
 * @brief 忙碌等待 (Busy-Wait) 函式
 */
void busy_wait(double seconds) {
    struct timespec start, current;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);

    while (1) {
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &current);
        double elapsed_sec = (current.tv_sec - start.tv_sec) + 
                             (current.tv_nsec - start.tv_nsec) / 1e9;
        
        if (elapsed_sec >= seconds) {
            break;
        }
    }
}

/**
 * @brief Worker 執行緒的進入點函式
 */
void *thread_func(void *arg) {
    thread_info_t *info = (thread_info_t *)arg;

    /* 1. 等待直到所有執行緒都準備好 (同步點) */
    pthread_barrier_wait(&barrier);

    /* 2. 執行任務 3 次 */
    for (int i = 0; i < 3; i++) {
        printf("Thread %d is running\n", info->id);
        
        /* 忙碌等待 <time_wait> 秒 */
        busy_wait(info->time_wait);
    }

    /* 3. 退出函式 */
    return NULL;
}

int main(int argc, char *argv[]) {
    int num_threads = 0;
    double time_wait = 0.0;
    char *policies_str = NULL;
    char *priorities_str = NULL;

    /* 1. 解析程式參數 (使用 getopt) */
    int opt;
    while ((opt = getopt(argc, argv, "n:t:s:p:")) != -1) {
        switch (opt) {
            case 'n':
                atoi(optarg) > 0 ? (num_threads = atoi(optarg)) : (num_threads = 0);
                break;
            case 't':
                atof(optarg) > 0 ? (time_wait = atof(optarg)) : (time_wait = 0.0);
                break;
            case 's':
                policies_str = optarg;
                break;
            case 'p':
                priorities_str = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s -n <num> -t <time> -s <policies> -p <priorities>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    /* 檢查參數是否已提供 */
    if (num_threads == 0 || time_wait == 0.0 || policies_str == NULL || priorities_str == NULL) {
        fprintf(stderr, "Missing or invalid arguments.\n");
        fprintf(stderr, "Usage: %s -n <num> -t <time> -s <policies> -p <priorities>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    /* 2. 綁定 CPU：將主執行緒 (以及它未來建立的所有子執行緒) 綁定到 CPU 0 */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == -1) {
        perror("sched_setaffinity");
        exit(EXIT_FAILURE);
    }

    /* 3. 初始化屏障 (Barrier) */
    pthread_barrier_init(&barrier, NULL, num_threads);

    /* 4. 準備執行緒屬性 */
    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    thread_info_t *thread_infos = malloc(num_threads * sizeof(thread_info_t));

    char *policies_str_dup = strdup(policies_str);
    char *priorities_str_dup = strdup(priorities_str);

    // 使用 strtok_r (可重入版本)，並宣告 saveptr 變數
    char *saveptr_policy;
    char *saveptr_priority;
    
    char *policy_token = strtok_r(policies_str_dup, ",", &saveptr_policy);
    char *priority_token = strtok_r(priorities_str_dup, ",", &saveptr_priority);


    for (int i = 0; i < num_threads; i++) {
        
        if (policy_token == NULL || priority_token == NULL) {
             fprintf(stderr, "Error: Mismatch in number of policies/priorities and num_threads.\n");
             exit(EXIT_FAILURE);
        }

        thread_infos[i].id = i;
        thread_infos[i].time_wait = time_wait;

        // --- 設定排程策略 ---
        if (strcmp(policy_token, "NORMAL") == 0) {
            thread_infos[i].policy = SCHED_OTHER; 
            thread_infos[i].priority = 0; 
        } else if (strcmp(policy_token, "FIFO") == 0) {
            thread_infos[i].policy = SCHED_FIFO;
            thread_infos[i].priority = atoi(priority_token);
        } else {
             fprintf(stderr, "Error: Unknown policy %s for thread %d.\n", policy_token, i);
             exit(EXIT_FAILURE);
        }

        // 移動到下一個 token (使用 strtok_r)
        policy_token = strtok_r(NULL, ",", &saveptr_policy);
        priority_token = strtok_r(NULL, ",", &saveptr_priority);

    }

    /* 5. 建立並啟動所有 Worker 執行緒 */
    for (int i = 0; i < num_threads; i++) {
        pthread_attr_t attr;
        struct sched_param param;

        pthread_attr_init(&attr);

        if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0) {
            perror("pthread_attr_setinheritsched");
        }
        
        if (pthread_attr_setschedpolicy(&attr, thread_infos[i].policy) != 0) {
             perror("pthread_attr_setschedpolicy");
        }
            
        param.sched_priority = thread_infos[i].priority;
        if (pthread_attr_setschedparam(&attr, &param) != 0) {
            perror("pthread_attr_setschedparam");
        }

        if (pthread_create(&threads[i], &attr, thread_func, &thread_infos[i]) != 0) {
            perror("pthread_create");
        }

        pthread_attr_destroy(&attr);
    }

    /* 6. 等待所有執行緒完成 */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    /* 7. 清理資源 */
    pthread_barrier_destroy(&barrier);
    free(threads);
    free(thread_infos);
    free(policies_str_dup);
    free(priorities_str_dup);

    return 0;
}