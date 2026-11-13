/* TCP回显应用线程入口；协议栈本身不直接调用应用的收发处理。 */

#ifndef MYPROGRAM_TCP_APP_H
#define MYPROGRAM_TCP_APP_H

/* 应用线程的TCP回显循环；监听事件对应accept，连接事件对应正文、EOF或错误。 */
int tcp_server_entry(void *arg);

#endif /* MYPROGRAM_TCP_APP_H */
