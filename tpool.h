/*
 * tpool.h - 轻量级线程池接口
 * 
 * 核心特性：
 *   - 固定大小线程池，线程数量在创建时确定
 *   - 环形任务队列，避免内存碎片和动态扩容开销
 *   - 条件变量 + 互斥锁，实现高效的线程休眠/唤醒
 *   - 支持优雅关闭和强制关闭两种模式
 *   - 线程安全设计，支持多线程并发访问
 * 
 * 编译要求：
 *   - 需要链接 pthread 库（-lpthread）
 * 
 * 使用方式：
 *   tpool_create() → tpool_add() → tpool_destroy()
 * 
 * 线程安全：
 *   - 所有共享数据访问都受互斥锁保护
 *   - 条件变量使用 while 循环检查条件，避免虚假唤醒
 */

#ifndef TPOOL_H
#define TPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 线程池前向声明
 * 
 * 隐藏内部实现细节，用户通过指针操作线程池。
 */
typedef struct tpool_t tpool_t;

/**
 * @brief 创建线程池
 * 
 * 创建并初始化线程池，包括：
 *   - 分配线程池结构体和任务队列内存
 *   - 初始化互斥锁和条件变量
 *   - 创建指定数量的工作线程
 * 
 * @param thread_count [输入] 工作线程数量（范围：1-100）
 * @param queue_size   [输入] 任务队列最大容量（范围：1-10000）
 * @return 线程池句柄（成功），NULL（失败）
 * 
 * @note 参数限制：
 *   - thread_count：1-100，避免线程过多占用系统资源
 *   - queue_size：1-10000，避免队列过大占用内存
 * 
 * @note 失败原因：
 *   - 参数不在有效范围内
 *   - 内存分配失败
 *   - 互斥锁/条件变量初始化失败
 *   - 线程创建失败
 */
tpool_t *tpool_create(int thread_count, int queue_size);

/**
 * @brief 向线程池添加任务
 * 
 * 将任务加入环形队列，并唤醒一个等待中的工作线程执行任务。
 * 
 * @param pool     [输入] 线程池句柄（由 tpool_create 返回）
 * @param function [输入] 任务函数指针，签名为 void (*function)(void *)
 * @param argument [输入] 传递给任务函数的参数，生命周期由调用者管理
 * @return 0 成功, -1 失败
 * 
 * @note 失败时 errno 可能的值：
 *   - EINVAL: 参数无效（pool 为 NULL 或 function 为 NULL）
 *   - EAGAIN: 任务队列已满
 *   - EPERM: 线程池已关闭（shutdown != 0）
 * 
 * @note 使用示例：
 *   void my_task(void *arg) {
 *       // 任务逻辑
 *   }
 *   tpool_add(pool, my_task, my_data);
 */
int tpool_add(tpool_t *pool, void (*function)(void *), void *argument);

/**
 * @brief 获取当前队列中等待的任务数
 * 
 * 返回线程池任务队列中尚未被工作线程取走的任务数量。
 * 
 * @param pool [输入] 线程池句柄
 * @return 当前等待的任务数量（成功），-1（失败，pool 为 NULL）
 * 
 * @note 此函数是线程安全的，内部使用互斥锁保护队列状态。
 */
int tpool_get_queue_size(tpool_t *pool);

/**
 * @brief 获取线程池线程总数
 * 
 * 返回线程池中的工作线程数量（与创建时指定的数量一致，除非创建过程中失败）。
 * 
 * @param pool [输入] 线程池句柄
 * @return 工作线程数量（成功），-1（失败，pool 为 NULL）
 * 
 * @note 此函数是线程安全的，内部使用互斥锁保护线程计数。
 */
int tpool_get_thread_count(tpool_t *pool);

/**
 * @brief 销毁线程池
 * 
 * 停止线程池并释放所有资源，支持两种关闭模式：
 *   - 优雅关闭（wait=1）：执行完队列中所有任务后退出
 *   - 强制关闭（wait=0）：立即退出，丢弃队列中剩余任务
 * 
 * @param pool [输入] 线程池句柄（由 tpool_create 返回）
 * @param wait [输入] 关闭模式：1-优雅关闭，0-强制关闭
 * @return 0 成功, -1 失败
 * 
 * @note 失败时 errno 可能的值：
 *   - EINVAL: 参数无效（pool 为 NULL）
 *   - EPERM: 线程池已被销毁（shutdown != 0）
 * 
 * @note 调用后 pool 变为无效，不应再使用。
 * @note 此函数是线程安全的，可在任意线程调用。
 */
int tpool_destroy(tpool_t *pool, int wait);

#ifdef __cplusplus
}
#endif

#endif // TPOOL_H