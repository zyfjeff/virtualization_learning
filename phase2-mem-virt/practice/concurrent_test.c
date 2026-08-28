#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define THREADS 4
#define SIZE_MB 50
#define ITERATIONS 20

void *memory_stress(void *arg) {
    int thread_id = *(int *)arg;
    char *mem = (char *)malloc(SIZE_MB * 1024 * 1024);
    
    if (!mem) {
        fprintf(stderr, "Thread %d: malloc failed\n", thread_id);
        return NULL;
    }
    
    printf("Thread %d: 开始内存压力测试 (%d MB)\n", thread_id, SIZE_MB);
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // 内存写入测试
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (size_t i = 0; i < SIZE_MB * 1024 * 1024; i += 4096) {
            mem[i] = (char)(thread_id + iter);
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;
    double speed = (SIZE_MB * ITERATIONS) / (elapsed / 1000.0);
    
    printf("Thread %d: 完成！速度: %.2f MB/s, 耗时: %.2f ms\n",
           thread_id, speed, elapsed);
    
    free(mem);
    return NULL;
}

int main() {
    printf("=== 并发内存压力测试 ===\n");
    printf("线程数: %d\n", THREADS);
    printf("每线程内存: %d MB\n", SIZE_MB);
    printf("总内存: %d MB\n", THREADS * SIZE_MB);
    printf("迭代次数: %d\n\n", ITERATIONS);
    
    pthread_t threads[THREADS];
    int thread_ids[THREADS];
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // 创建线程
    for (int i = 0; i < THREADS; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, memory_stress, &thread_ids[i]) != 0) {
            perror("pthread_create failed");
            return 1;
        }
    }
    
    // 等待所有线程完成
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;
    
    printf("\n=== 测试完成 ===\n");
    printf("总耗时: %.2f ms\n", elapsed);
    printf("平均速度: %.2f MB/s\n", (THREADS * SIZE_MB * ITERATIONS) / (elapsed / 1000.0));
    
    return 0;
}
