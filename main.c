/*
 * main.c - 高精度异步定时器库示例程序
 * 
 * 示例演示以下功能：
 *   1. 初始化定时器管理器（含线程池）
 *   2. 注册多个带别名的定时器
 *   3. 实时监控定时器和线程池状态
 *   4. 重置指定定时器的参数
 *   5. 取消指定定时器
 *   6. 优雅销毁管理器并释放资源
 * 
 * 使用流程：
 *   atimer_init() → atimer_register() → atimer_reset/atimer_cancel → atimer_destroy()
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include "atimer.h"

/**
 * @brief 定时器回调函数示例
 * 
 * 当定时器触发时，线程池会执行此回调函数。
 * 注意：回调函数执行在工作线程中，不应执行长时间阻塞操作。
 * 
 * @param param 用户注册时传入的参数（这里是 struct timespec*，表示定时器间隔）
 */
void high_precision_cb(void *param) {
    /** 获取当前线程 ID，用于调试和追踪 */
    pthread_t tid = pthread_self();
    
    /** 将参数转换为预期类型 */
    struct timespec *ts = (struct timespec *)param;
    
    /**
     * 打印定时器触发信息：
     *   - 线程 ID：标识执行回调的工作线程
     *   - 间隔时间：显示定时器的重复间隔（秒+纳秒）
     */
    printf("[Thread ID: %lu] Timer Fired! Interval: %ld.%09ld sec\n", 
           (unsigned long)tid, 
           ts->tv_sec, 
           ts->tv_nsec);
    
    /**
     * 模拟业务处理耗时：
     *   - 实际应用中，此处应放入真正的业务逻辑
     *   - 注意：长时间阻塞会占用工作线程，影响其他定时器的执行
     */
    sleep(1);
}

/**
 * @brief 主函数入口
 * 
 * 演示定时器管理器的完整使用流程：
 *   1. 初始化 → 2. 注册 → 3. 监控 → 4. 重置 → 5. 取消 → 6. 销毁
 */
int main() {
    /** 定时器管理器句柄，后续所有操作都需要此句柄 */
    atimer_mgr_t *mgr = NULL;
    
    /** 统计信息结构体，用于获取运行状态 */
    atimer_stats_t stats;
    
    /**
     * 步骤1：初始化定时器管理器
     *   - 参数1：输出参数，用于返回管理器句柄
     *   - 参数2：工作线程数量（建议设置为 CPU 核心数的 2-4 倍）
     *   - 参数3：任务队列最大容量（建议至少是定时器数量的 2 倍）
     */
    if (atimer_init(&mgr, 20, 1000) != 0) {
        perror("atimer_init failed");
        return 1;
    }

    /** 打印主线程 ID，用于区分主控制线程和工作线程 */
    printf("Manager initialized. Main Thread ID: %lu\n", (unsigned long)pthread_self());

    /**
     * 定义定时器时间参数：
     *   - interval：定时器重复间隔（500ms = 0秒 + 500,000,000纳秒）
     *   - delay：初始延迟（100ms = 0秒 + 100,000,000纳秒）
     */
    struct timespec interval;
    interval.tv_sec = 0;
    interval.tv_nsec = 500000000;

    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 100000000;

    /**
     * 定义定时器数组和别名：
     *   - timer_fd[3]：存储 3 个定时器的文件描述符（用于后续 reset/cancel）
     *   - timer_names[3]：定时器别名，便于识别和调试
     */
    int timer_fd[3];
    char timer_names[3][32] = {"Heartbeat-Timer", "Check-Timer", "Log-Timer"};

    /**
     * 步骤2：注册多个定时器
     *   - 使用循环注册 3 个定时器
     *   - 每个定时器都有唯一的别名和相同的时间参数
     *   - 返回值是定时器 fd，用于后续操作
     *   - 注册失败时跳过该定时器，继续注册下一个（不退出）
     */
    for (int i = 0; i < 3; i++) {
        timer_fd[i] = atimer_register(mgr, timer_names[i], &delay, &interval, 
                                       high_precision_cb, &interval);
        if (timer_fd[i] < 0) {
            fprintf(stderr, "Warning: timer '%s' register failed, skipping...\n", timer_names[i]);
            timer_fd[i] = -1;  /**< 标记为无效，后续操作跳过 */
            continue;          /**< 继续注册下一个定时器 */
        }
        printf("Registered timer '%s' with fd=%d\n", timer_names[i], timer_fd[i]);
    }

    printf("Timers registered. Waiting for callbacks...\n\n");

    /**
     * 步骤3：实时监控系统状态
     *   - 每隔 1 秒获取一次统计信息
     *   - 统计信息包括：活跃定时器数、等待任务数、工作线程数
     */
    for (int i = 0; i < 10; i++) {
        sleep(1);
        if (atimer_get_stats(mgr, &stats) == 0) {
            printf("--- Stats at %ds ---\n", i+1);
            printf("Active Timers: %d\n", stats.active_timers);
            printf("Pending Tasks: %d\n", stats.pending_tasks);
            printf("Worker Threads: %d\n\n", stats.worker_threads);
        }
    }

    /**
     * 步骤4：重置指定定时器
     *   - 将 Heartbeat-Timer 的间隔从 500ms 改为 2 秒
     *   - 使用 atimer_reset() 重新配置定时器参数
     *   - 参数：管理器句柄、定时器 fd、新的初始延迟、新的间隔
     *   - 若定时器注册失败则跳过重置操作
     */
    if (timer_fd[0] >= 0) {
        printf("Resetting timer '%s' (fd=%d) to 2 second interval...\n", timer_names[0], timer_fd[0]);
        struct timespec new_interval;
        new_interval.tv_sec = 2;
        new_interval.tv_nsec = 0;
        if (atimer_reset(mgr, timer_fd[0], &delay, &new_interval) != 0) {
            perror("atimer_reset failed");
        }
    } else {
        printf("Skipping reset for failed timer '%s'\n", timer_names[0]);
    }

    /** 等待 3 秒，观察重置后的定时器行为 */
    sleep(3);

    /**
     * 步骤5：取消指定定时器
     *   - 使用 atimer_cancel() 取消 Check-Timer 和 Log-Timer
     *   - 取消后，定时器停止触发，资源在 destroy 时统一释放
     *   - 若定时器注册失败则跳过取消操作
     */
    if (timer_fd[1] >= 0) {
        printf("Canceling timer '%s' (fd=%d)...\n", timer_names[1], timer_fd[1]);
        if (atimer_cancel(mgr, timer_fd[1]) != 0) {
            perror("atimer_cancel failed");
        }
    } else {
        printf("Skipping cancel for failed timer '%s'\n", timer_names[1]);
    }

    if (timer_fd[2] >= 0) {
        printf("Canceling timer '%s' (fd=%d)...\n", timer_names[2], timer_fd[2]);
        if (atimer_cancel(mgr, timer_fd[2]) != 0) {
            perror("atimer_cancel failed");
        }
    } else {
        printf("Skipping cancel for failed timer '%s'\n", timer_names[2]);
    }

    /**
     * 继续监控状态，验证取消操作的效果：
     *   - 活跃定时器数应从 3 减少到 1
     *   - 只有 Heartbeat-Timer 仍在触发
     */
    for (int i = 0; i < 5; i++) {
        sleep(1);
        if (atimer_get_stats(mgr, &stats) == 0) {
            printf("--- Stats at %ds ---\n", i+1);
            printf("Active Timers: %d\n", stats.active_timers);
            printf("Pending Tasks: %d\n", stats.pending_tasks);
            printf("Worker Threads: %d\n\n", stats.worker_threads);
        }
    }

    /**
     * 步骤6：销毁定时器管理器
     *   - 自动清理所有资源：线程池、定时器节点、epoll 实例等
     *   - 等待所有正在执行的任务完成后才释放资源
     */
    atimer_destroy(mgr);
    printf("Done. Resources released.\n");
    return 0;
}