/* 
 * atimer.c - Linux 高精度异步定时器库实现
 * 
 * 核心技术栈：
 *   - timerfd: 内核级高精度定时器，支持纳秒级精度
 *   - epoll: I/O 多路复用，边缘触发模式，高效监听大量定时器
 *   - pthread: POSIX 线程，用于管理线程和线程池
 *   - SCHED_FIFO: 实时调度策略，减少定时器触发延迟抖动
 * 
 * 架构设计：
 *   1. 单例管理线程 (manager_thread)：负责 epoll_wait 监听所有 timerfd
 *   2. 线程池 (tpool)：负责异步执行用户回调，避免阻塞管理线程
 *   3. 双链表设计：活跃链表 + zombie 链表，解决 cancel 与回调执行的竞态
 * 
 * 使用方式：
 *   atimer_init() → atimer_register() → atimer_reset/atimer_cancel → atimer_destroy()
 * 
 * 注意事项：
 *   - 必须定义 _GNU_SOURCE（在 Makefile 中配置）
 *   - 需要 root 权限或 CAP_SYS_NICE 才能启用 SCHED_FIFO
 *   - cancel 不会取消已提交到线程池的任务，只会阻止新任务提交
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <stdint.h>
#include <sched.h>

#include "atimer.h"
#include "tpool.h"

/**
 * @brief 定时器节点结构体
 * 
 * 每个注册的定时器对应一个节点，包含定时器描述符、别名、回调函数等信息。
 * 使用单向链表连接，支持快速插入和删除。
 */
typedef struct timer_node {
    int fd;                 /**< timerfd 文件描述符，作为定时器唯一标识 */
    char *name;             /**< 定时器别名（用户设置，便于调试和识别） */
    atimer_cb callback;     /**< 用户注册的回调函数指针 */
    void *param;            /**< 传递给回调函数的用户参数 */
    int cancelled;          /**< 取消标志：0-正常, 1-已取消（防止 UAF） */
    struct timer_node *next;/**< 链表下一个节点指针 */
} timer_node_t;

/**
 * @brief 定时器管理器结构体
 * 
 * 管理所有定时器资源，包括 epoll 实例、线程池、管理线程等。
 * 使用互斥锁保护共享数据，确保线程安全。
 */
struct atimer_mgr_t {
    int epoll_fd;           /**< epoll 实例句柄 */
    tpool_t *pool;          /**< 关联的线程池，用于异步执行回调 */
    pthread_t manager_thread;/**< 管理线程 ID，负责监听 epoll 事件 */
    int shutdown;           /**< 关闭标志：0-运行, 1-停止 */
    pthread_mutex_t lock;   /**< 互斥锁：保护链表、shutdown、timer_count */
    timer_node_t *head;     /**< 活跃定时器链表头 */
    timer_node_t *zombie_head;/**< 已取消定时器链表头（延迟释放，解决 UAF） */
    int timer_count;        /**< 当前活跃定时器数量 */
};

/**
 * @brief 线程池任务包装器
 * 
 * 作为线程池执行的任务函数，负责调用用户注册的回调函数。
 * 在执行前检查 cancelled 标志，防止访问已取消的定时器节点。
 * 
 * @param arg 定时器节点指针 (timer_node_t*)
 */
static void timer_wrapper(void *arg) {
    timer_node_t *node = (timer_node_t *)arg;
    if (!node || !node->callback) return;
    
    /**
     * 双重检查：即使在 atimer_cancel 之后提交到线程池的任务，
     * 也会在此处检测到 cancelled 标志并跳过执行。
     * 这是防止 Use-After-Free 的关键保护。
     */
    if (node->cancelled) return;
    
    /**
     * 调用用户回调函数，注意：
     *   1. 回调执行在工作线程中，不会阻塞管理线程
     *   2. 用户应避免在回调中执行长时间阻塞操作
     *   3. 用户参数 param 的生命周期由用户自行管理
     */
    node->callback(node->param);
}

/**
 * @brief 根据 fd 查找定时器节点
 * 
 * 在活跃链表中遍历查找指定 fd 对应的节点。
 * 注意：调用者需持有互斥锁。
 * 
 * @param mgr 管理器句柄
 * @param fd  定时器文件描述符
 * @return 找到返回节点指针，未找到返回 NULL
 */
static timer_node_t *find_node_by_fd(atimer_mgr_t *mgr, int fd) {
    if (!mgr) return NULL;
    timer_node_t *current = mgr->head;
    while (current) {
        if (current->fd == fd) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/**
 * @brief 管理线程主循环
 * 
 * 负责：
 *   1. 监听所有 timerfd 的触发事件
 *   2. 将触发的定时器回调任务提交到线程池
 *   3. 处理 shutdown 信号，优雅退出
 * 
 * 高优先级设计：
 *   - 尝试设置 SCHED_FIFO 实时调度策略
 *   - 使用 epoll 边缘触发模式，减少 CPU 占用
 *   - 批量处理事件（最多 10 个），提高效率
 * 
 * @param arg 管理器句柄 (atimer_mgr_t*)
 * @return NULL
 */
static void *manager_thread_func(void *arg) {
    atimer_mgr_t *mgr = (atimer_mgr_t *)arg;
    struct epoll_event events[10];  /**< 批量事件缓冲区，每次最多处理 10 个事件 */
    
    /**
     * 【高精度优化】尝试设置实时调度策略 SCHED_FIFO
     * 
     * SCHED_FIFO 特性：
     *   - 先进先出调度，一旦获得 CPU 就一直运行直到主动放弃
     *   - 显著减少因普通进程调度竞争带来的微秒级抖动
     *   - 需要 root 权限或 CAP_SYS_NICE 能力
     * 
     * 如果权限不足，记录警告但继续运行（使用默认调度策略）。
     */
    struct sched_param param;
    int max_prio = sched_get_priority_max(SCHED_FIFO);
    if (max_prio > 0) {
        param.sched_priority = max_prio / 2;  /**< 设置为中等实时优先级 */
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            fprintf(stderr, "Warning: Failed to set SCHED_FIFO. Running with default policy.\n");
        }
    }

    while (1) {
        /**
         * 检查关闭标志：
         *   - 使用短临界区，只读取 shutdown 值后立即释放锁
         *   - 避免长时间持锁导致其他操作阻塞
         */
        pthread_mutex_lock(&mgr->lock);
        int shut = mgr->shutdown;
        pthread_mutex_unlock(&mgr->lock);
        
        if (shut) break;  /**< 收到关闭信号，退出循环 */

        /**
         * 等待 epoll 事件：
         *   - timeout=100ms：平衡响应速度与 CPU 占用
         *   - 即使没有事件，每 100ms 检查一次 shutdown
         *   - 最多返回 10 个事件，批量处理提高效率
         */
        int n = epoll_wait(mgr->epoll_fd, events, 10, 100); 
        
        if (n == -1) {
            if (errno == EINTR) continue;  /**< 被信号中断，重试 */
            perror("epoll_wait");
            break;                         /**< 其他错误，退出循环 */
        }

        /**
         * 遍历处理所有触发的事件
         */
        for (int i = 0; i < n; i++) {
            /**
             * 通过 event.data.ptr 获取定时器节点指针
             * 这是在 atimer_register 时设置的
             */
            timer_node_t *node = (timer_node_t *)events[i].data.ptr;
            if (!node) continue;

            /**
             * 【线程安全】双重检查 cancelled 标志
             * 
             * 在检查和提交任务之间存在时间窗口，
             * 其他线程可能在此期间调用 atimer_cancel。
             * 通过在锁内检查 cancelled 标志，确保不会处理已取消的定时器。
             */
            pthread_mutex_lock(&mgr->lock);
            if (node->cancelled) {
                pthread_mutex_unlock(&mgr->lock);
                continue;
            }
            pthread_mutex_unlock(&mgr->lock);

            /**
             * 【关键步骤】读取 timerfd 以清除内核就绪状态
             * 
             * 在 ET (Edge Triggered) 模式下：
             *   - 必须读取 fd 以清除内核的就绪状态
             *   - 即使不关心触发次数，也必须 read
             *   - 否则 epoll 会一直返回该事件导致死循环
             * 
             * 读取的值 exp 表示定时器触发次数，但我们不使用它。
             */
            uint64_t exp;
            ssize_t s = read(node->fd, &exp, sizeof(uint64_t));
            if (s != sizeof(uint64_t)) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read timerfd");
                }
                continue;
            }

            /**
             * 将回调任务提交到线程池异步执行
             * 
             * 设计要点：
             *   - 管理线程立即返回继续监听，保证高精度定时不被耗时回调阻塞
             *   - 线程池负责执行回调，实现调度与执行解耦
             *   - 任务参数是 node 指针，timer_wrapper 会检查 cancelled 标志
             */
            tpool_add(mgr->pool, timer_wrapper, node);
        }
    }
    return NULL;
}

/**
 * @brief 初始化定时器管理器
 * 
 * 执行以下操作：
 *   1. 分配管理器内存
 *   2. 创建 epoll 实例
 *   3. 创建线程池
 *   4. 初始化互斥锁和链表
 *   5. 启动管理线程
 * 
 * @param mgr_out       输出：管理器句柄指针
 * @param thread_count  工作线程池中的线程数量（建议：CPU 核心数的 2-4 倍）
 * @param queue_size    任务队列的最大容量（建议：至少是定时器数量的 2 倍）
 * @return 0 成功, -1 失败（errno 可获取详细错误）
 */
int atimer_init(atimer_mgr_t **mgr_out, int thread_count, int queue_size) {
    /** 参数合法性检查 */
    if (!mgr_out || thread_count <= 0 || queue_size <= 0) return -1;

    /** 使用 calloc 分配内存，自动初始化为 0 */
    atimer_mgr_t *mgr = calloc(1, sizeof(atimer_mgr_t));
    if (!mgr) return -1;

    /**
     * 创建 epoll 实例：
     *   - EPOLL_CLOEXEC：exec 时自动关闭 fd，防止泄漏
     *   - 返回值为 epoll 文件描述符
     */
    mgr->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (mgr->epoll_fd == -1) {
        free(mgr);
        return -1;
    }

    /**
     * 创建线程池：
     *   - thread_count：工作线程数量
     *   - queue_size：任务队列最大容量
     */
    mgr->pool = tpool_create(thread_count, queue_size);
    if (!mgr->pool) {
        close(mgr->epoll_fd);
        free(mgr);
        return -1;
    }

    /** 初始化管理器状态 */
    mgr->shutdown = 0;
    mgr->head = NULL;
    mgr->zombie_head = NULL;
    mgr->timer_count = 0;
    
    /** 初始化互斥锁 */
    pthread_mutex_init(&mgr->lock, NULL);

    /**
     * 启动管理线程：
     *   - 线程函数：manager_thread_func
     *   - 传递参数：管理器句柄
     */
    if (pthread_create(&mgr->manager_thread, NULL, manager_thread_func, mgr) != 0) {
        /** 创建失败，清理已分配资源 */
        tpool_destroy(mgr->pool, 0);
        close(mgr->epoll_fd);
        pthread_mutex_destroy(&mgr->lock);
        free(mgr);
        return -1;
    }

    /** 输出管理器句柄 */
    *mgr_out = mgr;
    return 0;
}

/**
 * @brief 注册一个高精度定时器
 * 
 * 执行以下操作：
 *   1. 创建 timerfd
 *   2. 配置定时器参数（初始延迟、重复间隔）
 *   3. 创建定时器节点并初始化
 *   4. 将 fd 加入 epoll 监听
 *   5. 将节点加入活跃链表
 * 
 * @param mgr           管理器句柄
 * @param name          定时器别名（可为 NULL，用于标识定时器用途）
 * @param initial_delay 初始延迟时间 (struct timespec，秒+纳秒)
 * @param interval      重复间隔时间（若为 NULL 或 {0,0} 则仅执行一次）
 * @param callback      超时回调函数
 * @param param         传递给回调函数的用户参数
 * @return 成功返回定时器 fd（用于后续 reset/cancel）, -1 失败
 */
int atimer_register(atimer_mgr_t *mgr, const char *name, const struct timespec *initial_delay, 
                    const struct timespec *interval, atimer_cb callback, void *param) {
    /** 参数合法性检查 */
    if (!mgr || !callback || !initial_delay) return -1;

    /**
     * 创建 timerfd：
     *   - CLOCK_MONOTONIC：单调时钟，不受系统时间修改影响，适合计时
     *   - TFD_NONBLOCK：非阻塞模式，配合 epoll ET 使用
     *   - TFD_CLOEXEC：exec 时自动关闭 fd
     */
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd == -1) return -1;

    /** 配置定时器时间参数 */
    struct itimerspec new_value;
    memset(&new_value, 0, sizeof(new_value));
    
    /** 设置初始延迟（定时器首次触发的时间） */
    new_value.it_value.tv_sec = initial_delay->tv_sec;
    new_value.it_value.tv_nsec = initial_delay->tv_nsec;

    /** 设置重复间隔（0 表示一次性定时器） */
    if (interval) {
        new_value.it_interval.tv_sec = interval->tv_sec;
        new_value.it_interval.tv_nsec = interval->tv_nsec;
    }

    /**
     * 验证纳秒字段合法性：
     *   - tv_nsec 必须满足 0 <= tv_nsec < 1,000,000,000
     *   - 不合法则返回 EINVAL 错误
     */
    if (new_value.it_value.tv_nsec >= 1000000000L || 
        new_value.it_interval.tv_nsec >= 1000000000L) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    /** 应用定时器设置到内核 */
    if (timerfd_settime(fd, 0, &new_value, NULL) == -1) {
        close(fd);
        return -1;
    }

    /** 创建定时器节点 */
    timer_node_t *node = malloc(sizeof(timer_node_t));
    if (!node) {
        close(fd);
        return -1;
    }
    
    /** 初始化节点字段 */
    node->fd = fd;
    
    /**
     * 复制别名字符串：
     *   - 使用 strdup 动态分配内存，避免用户栈上字符串失效问题
     *   - 检查 strdup 返回值，防止内存不足导致的 NULL 指针
     */
    node->name = name ? strdup(name) : NULL;
    if (name && !node->name) {
        free(node);
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    
    node->callback = callback;
    node->param = param;
    node->cancelled = 0;  /**< 初始状态：未取消 */
    node->next = NULL;

    /**
     * 配置 epoll 事件：
     *   - EPOLLIN：监听读事件（timerfd 触发时可读）
     *   - EPOLLET：边缘触发模式，减少事件通知次数
     *   - event.data.ptr：关联定时器节点指针，便于事件处理时获取节点
     */
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    event.data.ptr = node;

    /** 将 fd 加入 epoll 监听 */
    if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) {
        free(node->name);
        free(node);
        close(fd);
        return -1;
    }

    /**
     * 将节点加入活跃链表：
     *   - 使用头插法，O(1) 时间复杂度
     *   - 必须在锁保护下操作，确保线程安全
     */
    pthread_mutex_lock(&mgr->lock);
    node->next = mgr->head;
    mgr->head = node;
    mgr->timer_count++;  /**< 更新活跃定时器计数 */
    pthread_mutex_unlock(&mgr->lock);

    /** 返回 timerfd 作为定时器标识 */
    return fd;
}

/**
 * @brief 重置指定的定时器
 * 
 * 修改定时器的初始延迟和重复间隔，相当于重新启动定时器。
 * 
 * @param mgr           管理器句柄
 * @param fd            定时器 fd（atimer_register 返回值）
 * @param initial_delay 新的初始延迟时间
 * @param interval      新的重复间隔时间
 * @return 0 成功, -1 失败（errno 可获取详细错误）
 */
int atimer_reset(atimer_mgr_t *mgr, int fd, const struct timespec *initial_delay, const struct timespec *interval) {
    /** 参数合法性检查 */
    if (!mgr || fd < 0 || !initial_delay) return -1;

    pthread_mutex_lock(&mgr->lock);
    
    /** 根据 fd 查找定时器节点 */
    timer_node_t *node = find_node_by_fd(mgr, fd);
    if (!node) {
        pthread_mutex_unlock(&mgr->lock);
        errno = ENOENT;  /**< 定时器不存在 */
        return -1;
    }

    /**
     * 检查 cancelled 标志：
     *   - 已取消的定时器不能重置
     *   - 返回 ENOENT，与"定时器不存在"语义一致
     */
    if (node->cancelled) {
        pthread_mutex_unlock(&mgr->lock);
        errno = ENOENT;
        return -1;
    }

    /** 配置新的定时器参数 */
    struct itimerspec new_value;
    memset(&new_value, 0, sizeof(new_value));
    
    new_value.it_value.tv_sec = initial_delay->tv_sec;
    new_value.it_value.tv_nsec = initial_delay->tv_nsec;

    if (interval) {
        new_value.it_interval.tv_sec = interval->tv_sec;
        new_value.it_interval.tv_nsec = interval->tv_nsec;
    }

    /** 验证纳秒字段合法性 */
    if (new_value.it_value.tv_nsec >= 1000000000L || 
        new_value.it_interval.tv_nsec >= 1000000000L) {
        pthread_mutex_unlock(&mgr->lock);
        errno = EINVAL;
        return -1;
    }

    /**
     * 在锁内调用 timerfd_settime：
     *   - 防止与 atimer_cancel 竞态
     *   - timerfd_settime 是内核调用，耗时极短，不会造成长时间阻塞
     */
    int ret = timerfd_settime(fd, 0, &new_value, NULL);
    pthread_mutex_unlock(&mgr->lock);

    return ret;
}

/**
 * @brief 取消指定的定时器
 * 
 * 执行以下操作：
 *   1. 从活跃链表中移除节点
 *   2. 设置 cancelled 标志
 *   3. 从 epoll 移除 fd
 *   4. 关闭 fd
 *   5. 将节点移入 zombie 链表（延迟释放）
 * 
 * 【线程安全设计】：
 *   - 不立即 free(node)，避免 Use-After-Free
 *   - 已提交到线程池的任务会检查 cancelled 标志
 *   - node 内存统一在 atimer_destroy 中释放
 * 
 * @param mgr 管理器句柄
 * @param fd  定时器 fd（atimer_register 返回值）
 * @return 0 成功, -1 失败（errno 可获取详细错误）
 */
int atimer_cancel(atimer_mgr_t *mgr, int fd) {
    /** 参数合法性检查 */
    if (!mgr || fd < 0) return -1;

    pthread_mutex_lock(&mgr->lock);
    
    /**
     * 检查 shutdown 标志：
     *   - 在 destroy 过程中不允许 cancel 操作
     *   - 防止与 destroy 竞态导致双重释放
     */
    if (mgr->shutdown) {
        pthread_mutex_unlock(&mgr->lock);
        errno = EPERM;
        return -1;
    }

    /**
     * 使用双指针法从链表中删除节点：
     *   - pp：指向当前节点指针的指针
     *   - 遍历链表找到 fd 匹配的节点
     */
    timer_node_t **pp = &mgr->head;
    timer_node_t *node = NULL;
    
    while (*pp) {
        if ((*pp)->fd == fd) {
            node = *pp;           /**< 保存要删除的节点 */
            *pp = node->next;     /**< 从链表中移除 */
            mgr->timer_count--;   /**< 更新活跃计数 */
            
            /**
             * 设置取消标志和无效 fd：
             *   - cancelled=1：通知其他线程该节点已取消
             *   - fd=-1：防止后续误操作
             */
            node->cancelled = 1;
            node->fd = -1;
            
            /**
             * 从 epoll 移除 fd：
             *   - 必须在锁内执行，防止管理线程继续处理该 fd 的事件
             *   - 移除后，即使 fd 仍有数据可读，epoll 也不会再通知
             */
            epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            
            /** 关闭 fd */
            close(fd);
            
            /**
             * 将节点移入 zombie 链表：
             *   - 延迟释放，解决与回调执行的竞态
             *   - 节点内存将在 atimer_destroy 中统一释放
             */
            node->next = mgr->zombie_head;
            mgr->zombie_head = node;
            
            break;
        }
        pp = &((*pp)->next);
    }

    pthread_mutex_unlock(&mgr->lock);

    /** 未找到指定 fd 的定时器 */
    if (!node) {
        errno = ENOENT;
        return -1;
    }

    return 0;
}

/**
 * @brief 获取当前系统运行状态统计
 * 
 * 获取以下信息：
 *   - 活跃定时器数量
 *   - 线程池中等待执行的任务数
 *   - 工作线程总数
 * 
 * @param mgr   管理器句柄
 * @param stats 输出统计信息结构体
 * @return 0 成功, -1 失败
 */
int atimer_get_stats(atimer_mgr_t *mgr, atimer_stats_t *stats) {
    if (!mgr || !stats) return -1;

    /** 获取活跃定时器数量（需加锁） */
    pthread_mutex_lock(&mgr->lock);
    stats->active_timers = mgr->timer_count;
    pthread_mutex_unlock(&mgr->lock);

    /** 获取线程池统计信息（tpool 内部已加锁） */
    stats->pending_tasks = tpool_get_queue_size(mgr->pool);
    stats->worker_threads = tpool_get_thread_count(mgr->pool);

    return 0;
}

/**
 * @brief 销毁定时器管理器并释放所有资源
 * 
 * 执行以下操作（按顺序）：
 *   1. 设置 shutdown 标志，通知管理线程退出
 *   2. 等待管理线程结束
 *   3. 销毁线程池（优雅关闭，等待任务执行完毕）
 *   4. 清理活跃链表（释放所有定时器节点）
 *   5. 清理 zombie 链表（释放已取消但未释放的节点）
 *   6. 关闭 epoll 实例和互斥锁
 *   7. 释放管理器内存
 * 
 * @param mgr 管理器句柄
 */
void atimer_destroy(atimer_mgr_t *mgr) {
    if (!mgr) return;

    /** 1. 设置 shutdown 标志，通知管理线程退出 */
    pthread_mutex_lock(&mgr->lock);
    mgr->shutdown = 1;
    pthread_mutex_unlock(&mgr->lock);

    /** 2. 等待管理线程结束 */
    pthread_join(mgr->manager_thread, NULL);

    /** 3. 销毁线程池（wait=1，优雅关闭） */
    tpool_destroy(mgr->pool, 1);

    /** 4. 清理活跃链表 */
    pthread_mutex_lock(&mgr->lock);
    timer_node_t *current = mgr->head;
    while (current) {
        timer_node_t *next = current->next;
        epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, current->fd, NULL);
        close(current->fd);
        free(current->name);
        free(current);
        current = next;
    }
    mgr->head = NULL;

    /** 5. 清理 zombie 链表 */
    current = mgr->zombie_head;
    while (current) {
        timer_node_t *next = current->next;
        free(current->name);
        free(current);
        current = next;
    }
    mgr->zombie_head = NULL;
    mgr->timer_count = 0;
    pthread_mutex_unlock(&mgr->lock);

    /** 6. 清理 epoll 和互斥锁 */
    close(mgr->epoll_fd);
    pthread_mutex_destroy(&mgr->lock);
    
    /** 7. 释放管理器内存 */
    free(mgr);
}