#ifndef MYPROGRAM_EVENT_POLL_H
#define MYPROGRAM_EVENT_POLL_H

#include <stdint.h>
#include <sys/epoll.h>

/* One user-space TCP readable-event instance; these are not Linux epoll calls. */
int event_poll_create(int size);
int event_poll_control(int epoll_fd, int operation, int socket_fd,
                       const struct epoll_event *event, uint32_t ready_events);
int event_poll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout_ms);
int event_poll_close(int epoll_fd);

/* TCP calls these while holding its registry mutex: TCP mutex -> poll mutex. */
void event_poll_update(int socket_fd, uint32_t ready_events);
void event_poll_remove(int socket_fd);

#endif /* MYPROGRAM_EVENT_POLL_H */
