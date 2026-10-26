#ifndef LOCAL_SERVER_H
#define LOCAL_SERVER_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>

#define IPC_MAX_CLIENTS 8
#define IPC_BUF_SIZE    4096
/* 套接字路径统一在 config.h 的 LOCAL_SOCK_PATH / LOCAL_SOCK_ENV 里配置 */

/* 单个客户端：独立输入缓冲区，按行解析NDJSON */
typedef struct {
    int         fd;
    char        inbuf[IPC_BUF_SIZE];
    size_t      inlen;
    bool        is_local;
} ipc_client_t;

/*
 * 本地UDS服务端主体（第8个线程管理对象）
 *
 * 不维护 client_cnt：槽位只认 fd，fd < 0 即空闲。
 * 计数字段和槽位数组是同一份状态的两份副本，任何一条关闭路径漏减
 * 都会让计数和实际连接数对不上，干脆只保留一个真实来源。
 */
typedef struct {
    int             listen_fd;
    int             epoll_fd;
    ipc_client_t    clients[IPC_MAX_CLIENTS];
    char            sock_path[sizeof(((struct sockaddr_un*)0)->sun_path)];
} local_server_t;

/* 接口声明 */

/* 成功返回 0；路径过长、bind/listen 失败返回 -1（errno 已设置） */
int  local_server_init(local_server_t *srv, const char *path);

/* 关闭所有连接、监听 fd，并 unlink 套接字文件 */
void local_server_destroy(local_server_t *srv);

/*
 * 跑一轮 epoll_wait。
 * 返回 0：正常（包括被 EINTR 打断，调用方回去检查 g_running 即可）
 * 返回 -1：epoll_wait 本身出错，线程应当退出
 */
int  local_server_run_once(local_server_t *srv, int timeout_ms);

/*
 * 向所有在线客户端广播一帧。
 * json_line 必须是一条完整的 NDJSON 行，末尾带 '\n'；
 * 函数不做补换行处理。
 *
 * 线程约束：srv 内部没有锁，run_once 和 broadcast 必须在同一个线程里调用
 * （也就是本地 IPC 线程自己），不能从 TCP 线程或显示线程跨线程调。
 */
void local_server_broadcast_snapshot(local_server_t *srv, const char *json_line);

#endif /* LOCAL_SERVER_H */
