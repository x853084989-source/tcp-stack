/*
 * 自制TCP可读通知：红黑树按fd查注册项，就绪链表枚举需要通知应用的项。
 * 事件项只描述就绪状态，不存TCP正文；正文仍由socket接收队列保存。
 */

#include "event_poll.h"

#include <errno.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#include <rte_malloc.h>

#include "third_party/nty_tree.h"
#include "config.h"

struct event_poll_item {
    /* 红黑树链接：按fd快速查注册项；它不携带报文正文。 */
    RB_ENTRY(event_poll_item) tree_link;

    /* 就绪链表链接：同一注册项只有满足订阅事件时才加入，不能借用TCP unack链接。 */
    TAILQ_ENTRY(event_poll_item) ready_link;

    int socket_fd;

    /* 是否已在就绪链表中，不是某个TCP包已到达的计数；用于防止重复入链。 */
    int ready;

    /* 真实状态与订阅事件交集，返回给应用的本轮可读标志。 */
    uint32_t ready_events;

    /* 应用注册时提供的订阅及data副本；data随事件返回，用于找回应用关心的fd。 */
    struct epoll_event event;
};

/* 红黑树比较回调：只按socket_fd排序，确保查找、插入与删除使用同一个键。 */
static int compare_socket_fd(struct event_poll_item *left, struct event_poll_item *right) {
    if (left->socket_fd < right->socket_fd) {
        return -1;
    }

    if (left->socket_fd > right->socket_fd) {
        return 1;
    }

    return 0;
}

RB_HEAD(event_poll_tree, event_poll_item);

/* 第三方宏生成本类型的树操作函数；compare_socket_fd决定排序，不是在这里创建线程或运行查找。 */
RB_GENERATE_INTERNAL(event_poll_tree, event_poll_item, tree_link, compare_socket_fd,
                     static __attribute__((unused)));
TAILQ_HEAD(event_poll_ready_list, event_poll_item);

struct event_poll_instance {
    struct event_poll_tree items;
    struct event_poll_ready_list ready_list;

    /* 当前就绪链表节点数，作为等待谓词；不是待接收字节数。 */
    unsigned ready_count;

    /* wait/close等操作持有的实例引用，阻塞期间也计入，避免条件变量提前销毁。 */
    unsigned references;
    int closed;
    pthread_cond_t cond;
};

/* One mutex protects the tree, ready predicates, waiters and instance lifetime. */
static pthread_mutex_t poll_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct event_poll_instance *poll_instance;

/* 构造只含fd键的临时key查红黑树；key不入树，返回的是已注册节点。 */
static struct event_poll_item *find_item(int socket_fd) {
    struct event_poll_item key = {.socket_fd = socket_fd};

    return RB_FIND(event_poll_tree, &poll_instance->items, &key);
}

/* 把真实可读状态与应用订阅取交集，再维护是否位于就绪链表；事件通知不搬运正文。 */
static void update_item(struct event_poll_item *item, uint32_t ready_events) {
    item->ready_events = ready_events & item->event.events;

    /* 从未就绪转为就绪才入链并唤醒；持续可读时不重复插入同一链接。 */
    if (item->ready_events != 0 && !item->ready) {
        item->ready = 1;
        TAILQ_INSERT_TAIL(&poll_instance->ready_list, item, ready_link);
        poll_instance->ready_count++;
        pthread_cond_broadcast(&poll_instance->cond);
    } else if (item->ready_events == 0 && item->ready) {
        /* 正文读空、状态变化或取消订阅后撤销就绪成员关系，否则应用会不断收到已失效事件。 */
        TAILQ_REMOVE(&poll_instance->ready_list, item, ready_link);
        item->ready = 0;
        poll_instance->ready_count--;
    }
}

/* 先撤销就绪成员关系，再摘除注册树节点，防止就绪链表留下悬空指针。 */
static void remove_item(struct event_poll_item *item) {
    update_item(item, 0);
    RB_REMOVE(event_poll_tree, &poll_instance->items, item);
    rte_free(item);
}

/* 检查本项目的单实例fd与关闭状态，调用者已持poll_mutex。 */
static int valid_instance(int epoll_fd) {
    if (epoll_fd != EVENT_POLL_FD || poll_instance == NULL || poll_instance->closed) {
        errno = EBADF;
        return 0;
    }

    return 1;
}

/* 等待者和close操作结束时释放引用，最后一个引用离开后才销毁已关闭的实例。 */
static void release_poll_reference(struct event_poll_instance *poll) {
    poll->references--;

    /* 关闭后仍可能有wait在cond中睡眠，只有最后一个操作离开才销毁实例和条件变量。 */
    if (poll->closed && poll->references == 0) {
        pthread_cond_destroy(&poll->cond);
        rte_free(poll);
        poll_instance = NULL;
    }
}

/* 创建注册树、就绪链表及条件变量；使用单调时钟以免校时改变等待期限。 */
int event_poll_create(int size) {
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&poll_mutex);
    if (poll_instance != NULL) {
        pthread_mutex_unlock(&poll_mutex);
        errno = EMFILE;
        return -1;
    }

    struct event_poll_instance *poll = rte_zmalloc("event_poll_instance", sizeof(*poll), 0);
    if (poll == NULL) {
        pthread_mutex_unlock(&poll_mutex);
        errno = ENOMEM;
        return -1;
    }

    pthread_condattr_t attributes;

    int error = pthread_condattr_init(&attributes);
    if (error == 0) {
        /* 指定timedwait使用单调时钟，避免系统日期校准导致超时突然提前/延后；deadline必须用同一时钟。 */
        error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
        if (error == 0) {
            error = pthread_cond_init(&poll->cond, &attributes);
        }

        pthread_condattr_destroy(&attributes);
    }

    if (error != 0) {
        rte_free(poll);
        pthread_mutex_unlock(&poll_mutex);
        errno = error;
        return -1;
    }

    RB_INIT(&poll->items);
    TAILQ_INIT(&poll->ready_list);
    poll_instance = poll;
    pthread_mutex_unlock(&poll_mutex);
    return EVENT_POLL_FD;
}

/* 执行ADD/MOD/DEL，并同步传入的当前就绪状态；这里只支持EPOLLIN订阅。 */
int event_poll_control(int epoll_fd, int operation, int socket_fd,
                       const struct epoll_event *event, uint32_t ready_events) {
    if ((operation != EPOLL_CTL_ADD && operation != EPOLL_CTL_MOD &&
         operation != EPOLL_CTL_DEL) ||
        (operation != EPOLL_CTL_DEL && event == NULL)) {
        errno = EINVAL;
        return -1;
    }

    /* 这里只实现可读通知；拒绝尚未实现的事件位，不能默默宣称支持EPOLLOUT/ET等语义。 */
    if (operation != EPOLL_CTL_DEL && (event->events & ~((uint32_t)EPOLLIN)) != 0) {
        errno = EOPNOTSUPP;
        return -1;
    }

    int error = 0;

    pthread_mutex_lock(&poll_mutex);
    if (!valid_instance(epoll_fd)) {
        pthread_mutex_unlock(&poll_mutex);
        return -1;
    }

    struct event_poll_item *item = find_item(socket_fd);
    if (operation == EPOLL_CTL_ADD) {
        if (item != NULL) {
            error = EEXIST;
        } else {
            item = rte_zmalloc("event_poll_item", sizeof(*item), 0);
            if (item == NULL) {
                error = ENOMEM;
            } else {
                item->socket_fd = socket_fd;
                item->event = *event;
                RB_INSERT(event_poll_tree, &poll_instance->items, item);
                update_item(item, ready_events);
            }
        }
    } else if (item == NULL) {
        error = ENOENT;
    } else if (operation == EPOLL_CTL_DEL) {
        remove_item(item);
    } else {
        item->event = *event;
        update_item(item, ready_events);
    }

    pthread_mutex_unlock(&poll_mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }

    return 0;
}

/* TCP状态变化时刷新已注册项，未订阅的fd无需创建事件节点。 */
void event_poll_update(int socket_fd, uint32_t ready_events) {
    pthread_mutex_lock(&poll_mutex);
    if (poll_instance != NULL && !poll_instance->closed) {
        struct event_poll_item *item = find_item(socket_fd);
        if (item != NULL) {
            update_item(item, ready_events);
        }
    }

    pthread_mutex_unlock(&poll_mutex);
}

/* 关闭端点时移除其注册与就绪项；不存在订阅则无需处理。 */
void event_poll_remove(int socket_fd) {
    pthread_mutex_lock(&poll_mutex);
    if (poll_instance != NULL && !poll_instance->closed) {
        struct event_poll_item *item = find_item(socket_fd);
        if (item != NULL) {
            remove_item(item);
        }
    }

    pthread_mutex_unlock(&poll_mutex);
}

/*
 * 复制最多max_events个就绪事件；不消费TCP正文，返回后由应用调用recv/accept。
 * 等待期间持有实例引用；cond等待会释放poll_mutex，醒来后重新检查状态。
 */
int event_poll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout_ms) {
    if (events == NULL || max_events <= 0) {
        errno = EINVAL;
        return -1;
    }

    /* deadline是本次wait的栈上绝对期限，tv_sec为秒、tv_nsec为纳秒；不是每次唤醒后再等timeout。 */
    struct timespec deadline = {0};
    if (timeout_ms > 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
            return -1;
        }

        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

        /* 毫秒余数换算纳秒后可能跨秒，归一化到tv_nsec小于10^9，满足timespec接口要求。 */
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&poll_mutex);
    if (!valid_instance(epoll_fd)) {
        pthread_mutex_unlock(&poll_mutex);
        return -1;
    }

    struct event_poll_instance *poll = poll_instance;
    poll->references++;

    int error = 0;

    /* 无就绪、未关闭且允许等待才睡眠；醒来后重查谓词，signal本身不保证事件仍存在。 */
    while (poll->ready_count == 0 && !poll->closed && timeout_ms != 0) {
        /* 负数无限等；正数timedwait使用预先计算的绝对deadline；0在while条件处直接跳过等待。 */
        if (timeout_ms < 0) {
            error = pthread_cond_wait(&poll->cond, &poll_mutex);
        } else {
            error = pthread_cond_timedwait(&poll->cond, &poll_mutex, &deadline);
        }

        if (error != 0) {
            break;
        }
    }

    int ready_count = 0;
    if (poll->closed) {
        error = EBADF;
    } else if (error == 0 || error == ETIMEDOUT) {
        /* 正常超时不是接口失败；无就绪项时返回0，临界时刻已有事件仍可返回它们；关闭则报告EBADF。 */
        error = 0;

        /* 就绪项返回后仍保留，因为正文尚未被recv消费；每轮移到尾部保证公平，用budget避免同次重复返回。 */
        unsigned budget = poll->ready_count;
        while (budget != 0 && ready_count < max_events) {
            struct event_poll_item *item = TAILQ_FIRST(&poll->ready_list);
            events[ready_count] = item->event;
            events[ready_count].events = item->ready_events;
            ready_count++;
            budget--;

            /* Keep level readiness, rotate for fairness, return each fd once per call. */
            TAILQ_REMOVE(&poll->ready_list, item, ready_link);
            TAILQ_INSERT_TAIL(&poll->ready_list, item, ready_link);
        }
    }

    release_poll_reference(poll);
    pthread_mutex_unlock(&poll_mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }

    return ready_count;
}

/* 标记关闭、删除注册项并唤醒等待者，延迟到所有引用释放后销毁条件变量。 */
int event_poll_close(int epoll_fd) {
    pthread_mutex_lock(&poll_mutex);
    if (!valid_instance(epoll_fd)) {
        pthread_mutex_unlock(&poll_mutex);
        return -1;
    }

    struct event_poll_instance *poll = poll_instance;
    poll->references++;
    poll->closed = 1;

    struct event_poll_item *item;
    while ((item = RB_MIN(event_poll_tree, &poll->items)) != NULL) {
        remove_item(item);
    }

    pthread_cond_broadcast(&poll->cond);
    release_poll_reference(poll);
    pthread_mutex_unlock(&poll_mutex);
    return 0;
}
