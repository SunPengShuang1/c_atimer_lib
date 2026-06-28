/*
 * atimer.h - 高精度异步定时器库接口
 * 
 * 核心特性：
 *   - 基于 Linux timerfd 实现纳秒级精度定时
 *   - 使用 epoll I/O 多路复用，高效监听大量定时器
 *   - 集成线程池，异步执行回调，不阻塞定时器管理线程
 *   - 支持定时器别名、重置、取消等完整操作
 *   - 线程安全设计，支持多线程并发访问
 * 
 * 编译要求：
 *   - 必须定义 _GNU_SOURCE（在 Makefile 中配置 -D_GNU_SOURCE）
 *   - 需要链接 pthread 库（-lpthread）
 * 
 * 使用限制：
 *   - 仅支持 Linux 系统（依赖 timerfd、epoll、SCHED_FIFO）
 *   - SCHED_FIFO 实时调度需要 root 权限或 CAP_SYS_NICE 能力
 *   - 回调函数不应执行长时间阻塞操作，以免占用工作线程
 */

#ifndef ATIMER_H
#define ATIMER_H

#include <stdint.h>
#include <time.h>   // 用于 struct timespec，支持纳秒级精度
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 定时器管理器前向声明
 * 
 * 隐藏内部实现细节，用户通过指针操作管理器。
 */
typedef struct atimer_mgr_t atimer_mgr_t;

/**
 * @brief 定时器回调函数类型定义
 * 
 * 用户注册定时器时需提供此签名的回调函数。
 * 回调函数执行在工作线程中，不应执行长时间阻塞操作。
 * 
 * @param param 用户注册时传入的自定义参数，生命周期由用户管理
 */
typedef void (*atimer_cb)(void *param);

/**
 * @brief 统计信息结构体
 * 
 * 用于获取定时器管理器的运行状态，便于监控和调试。
 */
typedef struct {
    int active_timers;    /**< 当前活跃的定时器个数（已注册且未取消） */
    int pending_tasks;    /**< 线程池中等待执行的任务数（定时器触发后尚未执行的回调） */
    int worker_threads;   /**< 工作线程总数（线程池中的线程数量） */
} atimer_stats_t;

/**
 * @brief 初始化定时器管理器
 * 
 * 创建并初始化定时器管理器，包括：
 *   - 创建 epoll 实例
 *   - 创建线程池
 *   - 启动管理线程（负责监听 epoll 事件）
 * 
 * @param mgr_out       [输出] 管理器句柄指针，成功时指向初始化后的管理器
 * @param thread_count  [输入] 工作线程池中的线程数量（建议：CPU 核心数的 2-4 倍，范围 1-100）
 * @param queue_size    [输入] 任务队列的最大容量（建议：至少是定时器数量的 2 倍，范围 1-10000）
 * @return 0 成功, -1 失败（errno 可获取详细错误原因）
 * 
 * @note 失败时 errno 可能的值：
 *   - EINVAL: 参数无效（thread_count <= 0 或 queue_size <= 0）
 *   - ENOMEM: 内存分配失败
 *   - EIO: 创建 epoll 实例或线程池失败
 */
int atimer_init(atimer_mgr_t **mgr_out, int thread_count, int queue_size);

/**
 * @brief 注册一个高精度定时器
 * 
 * 创建一个定时器并将其加入管理器，设置定时参数和回调函数。
 * 
 * @param mgr           [输入] 管理器句柄（由 atimer_init 返回）
 * @param name          [输入] 定时器别名（可为 NULL，用于标识定时器用途，便于调试）
 * @param initial_delay [输入] 初始延迟时间，即定时器首次触发的等待时间（struct timespec）
 * @param interval      [输入] 重复间隔时间（struct timespec）；若为 NULL 或 {0,0} 则仅执行一次
 * @param callback      [输入] 超时回调函数指针，定时器触发时由线程池调用
 * @param param         [输入] 传递给回调函数的用户参数，生命周期由用户管理
 * @return 成功返回定时器 fd（用于后续 atimer_reset/atimer_cancel）, -1 失败
 * 
 * @note 失败时 errno 可能的值：
 *   - EINVAL: 参数无效（mgr 为 NULL、callback 为 NULL、initial_delay 为 NULL 或纳秒值非法）
 *   - ENOMEM: 内存分配失败（包括 strdup 别名失败）
 *   - EIO: 创建 timerfd 失败或加入 epoll 失败
 * 
 * @note 使用示例：
 *   struct timespec delay = {0, 100000000};   // 初始延迟 100ms
 *   struct timespec interval = {1, 0};        // 重复间隔 1 秒
 *   int fd = atimer_register(mgr, "Heartbeat", &delay, &interval, my_callback, NULL);
 */
int atimer_register(atimer_mgr_t *mgr, const char *name, const struct timespec *initial_delay, 
                    const struct timespec *interval, atimer_cb callback, void *param);

/**
 * @brief 重置指定的定时器
 * 
 * 修改定时器的初始延迟和重复间隔，相当于重新启动定时器。
 * 已触发但尚未执行的回调不受影响。
 * 
 * @param mgr           [输入] 管理器句柄
 * @param fd            [输入] 定时器 fd（由 atimer_register 返回）
 * @param initial_delay [输入] 新的初始延迟时间
 * @param interval      [输入] 新的重复间隔时间（可为 NULL 表示一次性定时器）
 * @return 0 成功, -1 失败
 * 
 * @note 失败时 errno 可能的值：
 *   - EINVAL: 参数无效（mgr 为 NULL、fd < 0、initial_delay 为 NULL 或纳秒值非法）
 *   - ENOENT: 定时器不存在或已被取消
 *   - EIO: timerfd_settime 系统调用失败
 */
int atimer_reset(atimer_mgr_t *mgr, int fd, const struct timespec *initial_delay, 
                 const struct timespec *interval);

/**
 * @brief 取消指定的定时器
 * 
 * 停止定时器触发，从 epoll 中移除，并关闭 timerfd。
 * 已提交到线程池的任务会继续执行（通过 cancelled 标志保护）。
 * 
 * @param mgr [输入] 管理器句柄
 * @param fd  [输入] 定时器 fd（由 atimer_register 返回）
 * @return 0 成功, -1 失败
 * 
 * @note 失败时 errno 可能的值：
 *   - EINVAL: 参数无效（mgr 为 NULL、fd < 0）
 *   - ENOENT: 定时器不存在
 *   - EPERM: 管理器正在销毁中（destroy 过程中不允许 cancel）
 * 
 * @note 取消后的定时器资源不会立即释放，而是移入 zombie 链表，
 *       在 atimer_destroy 时统一释放，以避免 Use-After-Free 竞态条件。
 */
int atimer_cancel(atimer_mgr_t *mgr, int fd);

/**
 * @brief 获取当前系统运行状态统计
 * 
 * 获取定时器管理器和线程池的实时状态信息，用于监控和调试。
 * 
 * @param mgr   [输入] 管理器句柄
 * @param stats [输出] 统计信息结构体指针，成功时填充统计数据
 * @return 0 成功, -1 失败（参数无效时返回 -1）
 */
int atimer_get_stats(atimer_mgr_t *mgr, atimer_stats_t *stats);

/**
 * @brief 销毁定时器管理器并释放所有资源
 * 
 * 执行以下操作：
 *   1. 设置 shutdown 标志，通知管理线程退出
 *   2. 等待管理线程结束
 *   3. 销毁线程池（等待所有任务执行完毕）
 *   4. 释放所有定时器节点（包括活跃链表和 zombie 链表）
 *   5. 关闭 epoll 实例和互斥锁
 *   6. 释放管理器内存
 * 
 * @param mgr [输入] 管理器句柄（由 atimer_init 返回）
 * 
 * @note 此函数是线程安全的，可在任意线程调用。
 * @note 调用后 mgr 变为无效，不应再使用。
 */
void atimer_destroy(atimer_mgr_t *mgr);

#ifdef __cplusplus
}
#endif

#endif // ATIMER_H