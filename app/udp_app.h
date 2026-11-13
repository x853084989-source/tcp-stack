/* UDP应用线程入口；应用可阻塞等待，不承担网卡轮询或定时推进工作。 */

#ifndef MYPROGRAM_UDP_APP_H
#define MYPROGRAM_UDP_APP_H

/* Runs the UDP echo application's nsocket/nbind/nrecvfrom/nsendto loop. */
/* 应用线程的UDP回显循环；接收长度与来源地址决定回显内容和目标，不按字符串处理正文。 */
int udp_server_entry(void *arg);

#endif /* MYPROGRAM_UDP_APP_H */
