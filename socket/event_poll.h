/* 单实例、仅EPOLLIN的用户态通知接口，不能等同Linux epoll的完整功能。 */

#ifndef MYPROGRAM_EVENT_POLL_H
#define MYPROGRAM_EVENT_POLL_H

#include <stdint.h>
#include <sys/epoll.h>

/* One user-space TCP readable-event instance; these are not Linux epoll calls. */
/* 创建注册树、就绪链表及条件变量；使用单调时钟以免校时改变等待期限。 */
int event_poll_create(int size);

/* 执行ADD/MOD/DEL，并同步传入的当前就绪状态；这里只支持EPOLLIN订阅。 */
int event_poll_control(int epoll_fd, int operation, int socket_fd,
                       const struct epoll_event *event, uint32_t ready_events);

/* 复制最多max_events个就绪事件；不消费TCP正文，返回后由应用调用recv/accept。 */
int event_poll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout_ms);

/* 标记关闭、删除注册项并唤醒等待者，延迟到所有引用释放后销毁条件变量。 */
int event_poll_close(int epoll_fd);

/* TCP calls these while holding its registry mutex: TCP mutex -> poll mutex. */
/* TCP状态变化时刷新已注册项，未订阅的fd无需创建事件节点。 */
void event_poll_update(int socket_fd, uint32_t ready_events);

/* 关闭端点时移除其注册与就绪项；不存在订阅则无需处理。 */
void event_poll_remove(int socket_fd);

#endif /* MYPROGRAM_EVENT_POLL_H */
