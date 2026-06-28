/*
 * tpool.c - 轻量级线程池实现
 * 
 * 设计特点：
 *   - 固定大小线程池，线程数量在创建时确定
 *   - 环形任务队列，避免内存碎片和动态扩容开销
 *   - 条件变量 + 互斥锁，实现高效的线程休眠/唤醒
 *   - 支持优雅关闭和强制关闭两种模式
 * 
 * 核心数据结构：
 *   - tpool_t：线程池主结构体
 *   - tpool_task_t：任务结构体（函数指针 + 参数）
 * 
 * 使用方式：
 *   tpool_create() → tpool_add() → tpool_destroy()
 * 
 * 线程安全：
 *   - 所有共享数据访问都受互斥锁保护
 *   - 条件变量使用 while 循环检查条件，避免虚假唤醒
 */

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include "tpool.h"

/**
 * @brief 任务结构体
 * 
 * 每个任务包含一个函数指针和对应的参数，
 * 工作线程从队列中取出任务后执行。
 */
typedef struct {
    void (*function)(void *);  /**< 任务函数指针 */
    void *argument;            /**< 传递给任务函数的参数 */
} tpool_task_t;

/**
 * @brief 线程池结构体
 * 
 * 管理线程池的所有资源，包括工作线程、任务队列和同步原语。
 */
struct tpool_t {
    pthread_mutex_t lock;       /**< 互斥锁：保护队列、线程计数、shutdown 标志 */
    pthread_cond_t notify;      /**< 条件变量：通知工作线程有新任务或退出 */
    pthread_t *threads;         /**< 工作线程 ID 数组 */
    tpool_task_t *queue;        /**< 环形任务队列 */
    int thread_count;           /**< 当前工作线程数量（与创建时可能不同，若创建失败） */
    int queue_size;             /**< 任务队列最大容量 */
    int head;                   /**< 队头索引：下一个写入位置 */
    int tail;                   /**< 队尾索引：下一个读取位置 */
    int count;                  /**< 当前队列中的任务数量 */
    int shutdown;               /**< 关闭标志：0-运行, 1-优雅关闭, 2-强制关闭 */
};

/**
 * @brief 工作线程入口函数
 * 
 * 工作线程循环执行以下操作：
 *   1. 加锁，检查队列是否为空且未关闭
 *   2. 队列为空且未关闭时，等待条件变量信号
 *   3. 收到信号后，检查关闭标志和队列状态
 *   4. 若未关闭且队列有任务，取出任务
 *   5. 解锁，执行任务（锁外执行，提高并发）
 * 
 * 关闭行为：
 *   - shutdown==1（优雅关闭）：执行完队列中所有任务后退出
 *   - shutdown==2（强制关闭）：立即退出，丢弃队列中剩余任务
 * 
 * @param arg 线程池句柄 (tpool_t*)
 * @return NULL
 */
static void *tpool_worker(void *arg) {
    tpool_t *pool = (tpool_t *)arg;
    tpool_task_t task;  /**< 任务缓冲区 */

    while (1) {
        /** 获取互斥锁 */
        pthread_mutex_lock(&(pool->lock));

        /**
         * 等待条件变量：
         *   - 使用 while 循环而非 if，防止虚假唤醒
         *   - 条件：队列为空 且 未关闭
         *   - 满足条件时，线程进入休眠，释放锁
         *   - 被唤醒后，重新获取锁并检查条件
         */
        while ((pool->count == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

        /**
         * 强制关闭检查（shutdown==2）：
         *   - 立即退出，不执行队列中剩余任务
         *   - 适用于需要快速停止的场景
         */
        if (pool->shutdown == 2) {
            pthread_mutex_unlock(&(pool->lock));
            pthread_exit(NULL);
        }

        /**
         * 优雅关闭检查（shutdown==1）：
         *   - 执行完队列中所有任务后退出
         *   - 条件：已关闭 且 队列为空
         */
        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&(pool->lock));
            pthread_exit(NULL);
        }

        /**
         * 从队列尾部取出任务：
         *   - tail 指向队列中下一个待执行的任务
         *   - 任务的 function 和 argument 拷贝到局部变量 task
         */
        task.function = pool->queue[pool->tail].function;
        task.argument = pool->queue[pool->tail].argument;
        
        /**
         * 移动尾指针：
         *   - 使用取模运算实现环形队列
         *   - tail = (tail + 1) % queue_size
         */
        pool->tail = (pool->tail + 1) % pool->queue_size;
        pool->count--;  /**< 任务数量减一 */

        /** 释放互斥锁 */
        pthread_mutex_unlock(&(pool->lock));

        /**
         * 在锁外执行任务：
         *   - 避免长时间持锁，提高并发性能
         *   - 多个工作线程可以同时执行不同任务
         *   - 用户应确保任务函数线程安全
         */
        (*(task.function))(task.argument);
    }
    return NULL;
}

/**
 * @brief 创建线程池
 * 
 * 执行以下操作：
 *   1. 参数合法性检查
 *   2. 分配线程池结构体内存
 *   3. 分配线程数组和任务队列内存
 *   4. 初始化互斥锁和条件变量
 *   5. 创建工作线程
 * 
 * @param thread_count 线程数量（范围：1-100）
 * @param queue_size   队列最大容量（范围：1-10000）
 * @return 线程池句柄（失败返回 NULL）
 */
tpool_t *tpool_create(int thread_count, int queue_size) {
    tpool_t *pool;
    int i;

    /**
     * 参数合法性检查：
     *   - thread_count：1-100，避免线程过多占用资源
     *   - queue_size：1-10000，避免队列过大占用内存
     */
    if (thread_count <= 0 || thread_count > 100 || queue_size <= 0 || queue_size > 10000) {
        return NULL;
    }

    /** 分配线程池结构体内存 */
    pool = (tpool_t *)malloc(sizeof(tpool_t));
    if (pool == NULL) goto err;

    /** 初始化线程池状态 */
    pool->thread_count = 0;  /**< 当前线程数，初始为 0 */
    pool->queue_size = queue_size;
    pool->head = pool->tail = pool->count = 0;  /**< 队列为空 */
    pool->shutdown = 0;  /**< 运行状态 */

    /**
     * 分配线程数组和任务队列内存：
     *   - threads：存储工作线程 ID
     *   - queue：环形任务队列
     */
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    pool->queue = (tpool_task_t *)malloc(sizeof(tpool_task_t) * queue_size);

    /** 检查内存分配是否成功 */
    if (pool->threads == NULL || pool->queue == NULL) goto err;

    /**
     * 初始化同步原语：
     *   - lock：互斥锁，保护共享数据
     *   - notify：条件变量，用于线程间通信
     */
    if (pthread_mutex_init(&(pool->lock), NULL) != 0 ||
        pthread_cond_init(&(pool->notify), NULL) != 0) {
        goto err;
    }

    /**
     * 创建工作线程：
     *   - 循环创建 thread_count 个线程
     *   - 每个线程执行 tpool_worker 函数
     *   - 如果创建失败，调用 tpool_destroy 清理已创建资源
     */
    for (i = 0; i < thread_count; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, tpool_worker, (void *)pool) != 0) {
            tpool_destroy(pool, 0);  /**< 创建失败，强制关闭并清理 */
            return NULL;
        }
        pool->thread_count++;  /**< 成功创建一个线程，计数加一 */
    }

    /** 返回线程池句柄 */
    return pool;

/**
 * 错误处理：清理已分配资源
 *   - 如果 threads 已分配，释放
 *   - 如果 queue 已分配，释放
 *   - 释放线程池结构体
 */
err:
    if (pool) {
        if (pool->threads) free(pool->threads);
        if (pool->queue) free(pool->queue);
        free(pool);
    }
    return NULL;
}

/**
 * @brief 向线程池添加任务
 * 
 * 将任务加入队列，并唤醒一个等待中的工作线程。
 * 
 * @param pool     线程池句柄
 * @param function 任务函数指针
 * @param argument 任务参数
 * @return 0 成功, -1 失败（队列已满或池已关闭）
 */
int tpool_add(tpool_t *pool, void (*function)(void *), void *argument) {
    int err = 0;
    int next;  /**< 下一个写入位置 */

    /** 参数合法性检查 */
    if (pool == NULL || function == NULL) return -1;

    /** 获取互斥锁 */
    pthread_mutex_lock(&(pool->lock));

    /**
     * 计算下一个写入位置：
     *   - head 指向当前可写入位置
     *   - next = (head + 1) % queue_size
     *   - 如果 next == tail，说明队列已满
     */
    next = (pool->head + 1) % pool->queue_size;
    
    /**
     * 检查是否可以添加任务：
     *   - 队列已满（count == queue_size）：拒绝添加
     *   - 池已关闭（shutdown != 0）：拒绝添加
     */
    if (pool->count == pool->queue_size || pool->shutdown) {
        err = -1;
        goto out;
    }

    /**
     * 将任务放入队列：
     *   - 存储函数指针和参数到队列的 head 位置
     *   - 更新 head 指针
     *   - 任务数量加一
     */
    pool->queue[pool->head].function = function;
    pool->queue[pool->head].argument = argument;
    pool->head = next;
    pool->count++;

    /**
     * 唤醒一个等待中的工作线程：
     *   - 使用 signal 而非 broadcast，避免惊群效应
     *   - 只需唤醒一个线程处理新任务
     */
    pthread_cond_signal(&(pool->notify));

/**
 * 退出路径：释放互斥锁
 */
out:
    pthread_mutex_unlock(&(pool->lock));
    return err;
}

/**
 * @brief 获取当前队列中等待的任务数
 * 
 * @param pool 线程池句柄
 * @return 任务数量（失败返回 -1）
 */
int tpool_get_queue_size(tpool_t *pool) {
    if (!pool) return -1;
    
    /** 加锁读取任务数量 */
    pthread_mutex_lock(&(pool->lock));
    int count = pool->count;
    pthread_mutex_unlock(&(pool->lock));
    
    return count;
}

/**
 * @brief 获取线程池线程总数
 * 
 * @param pool 线程池句柄
 * @return 线程数量（失败返回 -1）
 */
int tpool_get_thread_count(tpool_t *pool) {
    if (!pool) return -1;
    
    /**
     * 加锁读取线程数量：
     *   - 虽然 thread_count 在创建后不再变化，
     *   - 但为了保持代码一致性和潜在的扩展性，仍加锁保护
     */
    pthread_mutex_lock(&(pool->lock));
    int count = pool->thread_count;
    pthread_mutex_unlock(&(pool->lock));
    
    return count;
}

/**
 * @brief 销毁线程池
 * 
 * 执行以下操作：
 *   1. 设置关闭标志（优雅或强制）
 *   2. 广播通知所有工作线程退出
 *   3. 等待所有工作线程结束
 *   4. 释放所有资源
 * 
 * @param pool 线程池句柄
 * @param wait 1: 优雅关闭（执行完剩余任务）; 0: 强制关闭（立即退出）
 * @return 0 成功, -1 失败（池已销毁或参数无效）
 */
int tpool_destroy(tpool_t *pool, int wait) {
    int i, err = 0;
    
    /** 参数合法性检查 */
    if (pool == NULL) return -1;

    /** 获取互斥锁 */
    pthread_mutex_lock(&(pool->lock));
    
    /**
     * 检查是否已销毁：
     *   - 如果 shutdown != 0，说明已调用过 destroy
     *   - 防止重复销毁
     */
    if (pool->shutdown) {
        pthread_mutex_unlock(&(pool->lock));
        return -1;
    }

    /**
     * 设置关闭标志：
     *   - wait == 1：shutdown = 1（优雅关闭）
     *   - wait == 0：shutdown = 2（强制关闭）
     */
    pool->shutdown = (wait) ? 1 : 2;
    
    /**
     * 广播通知所有等待线程：
     *   - 使用 broadcast 而非 signal
     *   - 需要唤醒所有线程检查关闭标志
     */
    pthread_cond_broadcast(&(pool->notify));
    
    /** 释放互斥锁 */
    pthread_mutex_unlock(&(pool->lock));

    /**
     * 等待所有工作线程结束：
     *   - 循环调用 pthread_join
     *   - 确保所有线程都已退出
     */
    for (i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    /**
     * 释放资源：
     *   - threads：线程 ID 数组
     *   - queue：任务队列
     *   - lock：互斥锁
     *   - notify：条件变量
     *   - pool：线程池结构体
     */
    free(pool->threads);
    free(pool->queue);
    pthread_mutex_destroy(&(pool->lock));
    pthread_cond_destroy(&(pool->notify));
    free(pool);

    return err;
}