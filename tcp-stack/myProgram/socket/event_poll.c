#include "event_poll.h"

#include <errno.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>

#include <rte_malloc.h>

#include "common/nty_tree.h"
#include "config.h"

struct event_poll_item {
    RB_ENTRY(event_poll_item) tree_link;
    TAILQ_ENTRY(event_poll_item) ready_link;
    int socket_fd;
    int ready;
    uint32_t ready_events;
    struct epoll_event event;
};

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
RB_GENERATE_INTERNAL(event_poll_tree, event_poll_item, tree_link, compare_socket_fd,
                     static __attribute__((unused)));
TAILQ_HEAD(event_poll_ready_list, event_poll_item);

struct event_poll_instance {
    struct event_poll_tree items;
    struct event_poll_ready_list ready_list;
    unsigned ready_count;
    unsigned references;
    int closed;
    pthread_cond_t cond;
};

/* One mutex protects the tree, ready predicates, waiters and instance lifetime. */
static pthread_mutex_t poll_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct event_poll_instance *poll_instance;

static struct event_poll_item *find_item(int socket_fd) {
    struct event_poll_item key = {.socket_fd = socket_fd};
    return RB_FIND(event_poll_tree, &poll_instance->items, &key);
}

static void update_item(struct event_poll_item *item, uint32_t ready_events) {
    item->ready_events = ready_events & item->event.events;
    if (item->ready_events != 0 && !item->ready) {
        item->ready = 1;
        TAILQ_INSERT_TAIL(&poll_instance->ready_list, item, ready_link);
        poll_instance->ready_count++;
        pthread_cond_broadcast(&poll_instance->cond);
    } else if (item->ready_events == 0 && item->ready) {
        TAILQ_REMOVE(&poll_instance->ready_list, item, ready_link);
        item->ready = 0;
        poll_instance->ready_count--;
    }
}

static void remove_item(struct event_poll_item *item) {
    update_item(item, 0);
    RB_REMOVE(event_poll_tree, &poll_instance->items, item);
    rte_free(item);
}

static int valid_instance(int epoll_fd) {
    if (epoll_fd != EVENT_POLL_FD || poll_instance == NULL || poll_instance->closed) {
        errno = EBADF;
        return 0;
    }
    return 1;
}

static void release_poll_reference(struct event_poll_instance *poll) {
    poll->references--;
    if (poll->closed && poll->references == 0) {
        pthread_cond_destroy(&poll->cond);
        rte_free(poll);
        poll_instance = NULL;
    }
}

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

int event_poll_control(int epoll_fd, int operation, int socket_fd,
                       const struct epoll_event *event, uint32_t ready_events) {
    if ((operation != EPOLL_CTL_ADD && operation != EPOLL_CTL_MOD &&
         operation != EPOLL_CTL_DEL) ||
        (operation != EPOLL_CTL_DEL && event == NULL)) {
        errno = EINVAL;
        return -1;
    }
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

int event_poll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout_ms) {
    if (events == NULL || max_events <= 0) {
        errno = EINVAL;
        return -1;
    }
    struct timespec deadline = {0};
    if (timeout_ms > 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
            return -1;
        }
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
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
    while (poll->ready_count == 0 && !poll->closed && timeout_ms != 0) {
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
        error = 0;
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
