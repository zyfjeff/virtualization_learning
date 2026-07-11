#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIZE_MB 100
#define ITERATIONS 50

double get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main() {
    printf("=== VM 内内存测试 ===\n");
    printf("分配 %d MB 内存\n", SIZE_MB);
    
    char *mem = (char *)malloc(SIZE_MB * 1024 * 1024);
    if (!mem) {
        perror("malloc failed");
        return 1;
    }
    
    // 初始化内存
    printf("初始化内存...\n");
    memset(mem, 0x55, SIZE_MB * 1024 * 1024);
    
    // 内存访问测试
    double start = get_time_ms();
    long long sum = 0;
    
    for (int iter = 0; iter < ITERATIONS; iter++) {
        for (size_t i = 0; i < SIZE_MB * 1024 * 1024; i += 64) {
            sum += mem[i];
        }
    }
    
    double end = get_time_ms();
    double speed = (SIZE_MB * ITERATIONS) / ((end - start) / 1000.0);
    
    printf("测试完成！\n");
    printf("速度: %.2f MB/s\n", speed);
    printf("耗时: %.2f ms\n", end - start);
    
    free(mem);
    return 0;
}
