/*
 * local_server.c — 本地 Unix 域套接字服务端（给板端 Qt 界面用）
 *
 * 传输：AF_UNIX + SOCK_STREAM，Qt 侧用 QLocalSocket 连同一个路径。
 * 协议：NDJSON，一行一条 JSON，帧尾 '\n'。
 *
 *   服务端 → 客户端（推送，2Hz）：
 *     {"t":...,"mpu":{...},"ppg":{...}}\n
 *
 *   客户端 → 服务端（命令）：
 *     {"seq":1,"cmd":"ping"}\n
 *   服务端 → 客户端（响应）：
 *     {"seq":1,"ok":true}\n
 *     {"seq":1,"ok":false,"err":"unknown cmd"}\n
 *
 *   Qt 侧按"有没有 seq"区分响应和推送；按 cmd 扩展具体命令。
 *
 * 触发方式：水平触发（LT）。本地 UI 只有 2Hz，LT 不需要"每次事件必须
 *       recv 到 EAGAIN"的约束，少一层出错机会。
 */

/*
 * globals.h 必须排在所有系统头文件之前。
 * 它第一行 include 的是 config.h，而 config.h 里定义了 _GNU_SOURCE；
 * 这些特性宏只有在 <features.h> 第一次被处理之前生效。若先 include
 * local_server.h（它带 <sys/socket.h>），features.h 会先跑一遍，
 * _GNU_SOURCE 就晚了 —— accept4 / SOCK_CLOEXEC 都不会有声明。
 */
#include "globals.h"
#include "local_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>      /* fcntl 设置非阻塞 */
#include <sys/epoll.h>
#include <sys/stat.h>   /* chmod */

#include "mpu6050.h"
#include "max30102.h"
#include "cJSON.h"

#define IPC_MAX_EVENTS 16

static void handle_new_conn(local_server_t *srv);
static void handle_client_read(local_server_t *srv, ipc_client_t *cli);
static void close_client(local_server_t *srv, ipc_client_t *cli);
static int  send_frame(ipc_client_t *cli, const char *line, size_t len);
static void send_snapshot_to(local_server_t *srv, ipc_client_t *cli);
static int  handle_client_line(ipc_client_t *cli, const char *line, size_t len);
static int  send_response(ipc_client_t *cli, double seq, int ok, const char *err);

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int local_server_init(local_server_t *srv, const char *path)
{
    if (!srv || !path)
    {
        errno = EINVAL;
        return -1;
    }

    srv->listen_fd    = -1;
    srv->epoll_fd     = -1;
    srv->sock_path[0] = '\0';   /* init 失败时 destroy 不会去 unlink 垃圾路径 */

    /*
     * 空槽位统一用 fd = -1 标记。
     * 原来 memset 成 0 也能跑（0 是 stdin），但语义含糊：fd <= 0 的判断
     * 把"空闲槽"和"占用了 fd 0"混在一起，后面维护容易出错。
     */
    for (int i = 0; i < IPC_MAX_CLIENTS; i++)
    {
        srv->clients[i].fd       = -1;
        srv->clients[i].inlen    = 0;
        srv->clients[i].is_local = false;
    }

    /* sun_path 放不下就明确失败，别截断 */
    if (strlen(path) >= sizeof(srv->sock_path))
    {
        errno = ENAMETOOLONG;
        perror("local_server: path too long");
        return -1;
    }
    strcpy(srv->sock_path, path);   /* 长度已校验 */

    /*
     * 上次异常退出会留下 socket 文件，bind 会返回 EADDRINUSE。
     * 只删 socket 类型的文件，避免把同名的普通文件误删。
     */
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode))
    {
        unlink(path);
    }

    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv->listen_fd < 0)
    {
        perror("local_server: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                     + strlen(path) + 1);
    if (bind(srv->listen_fd, (const struct sockaddr *)&addr, addr_len) < 0)
    {
        perror("local_server: bind");
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    /*
     * 套接字文件的权限决定"谁能 connect"：
     * connect 需要对 socket 文件有写权限，root 用 umask 022 建出来是 0755，
     * 普通用户（debian）会被 EACCES 挡住。这里放宽到 0660，
     * 同用户 / 同组都能连。失败不致命，但 Qt 侧可能连不上，要留日志。
     */
    if (chmod(path, 0660) < 0)
    {
        perror("local_server: chmod");
    }

    if (listen(srv->listen_fd, IPC_MAX_CLIENTS) < 0)
    {
        perror("local_server: listen");
        goto fail;
    }
    if (set_nonblock(srv->listen_fd) < 0)
    {
        perror("local_server: set_nonblock");
        goto fail;
    }

    srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv->epoll_fd < 0)
    {
        perror("local_server: epoll_create1");
        goto fail;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = srv->listen_fd;
    ev.events  = EPOLLIN;
    if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev) < 0)
    {
        perror("local_server: epoll_ctl");
        goto fail;
    }

    return 0;

fail:
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    if (srv->epoll_fd  >= 0) close(srv->epoll_fd);
    srv->listen_fd = srv->epoll_fd = -1;
    unlink(path);
    return -1;
}

int local_server_run_once(local_server_t *srv, int timeout_ms)
{
    struct epoll_event events[IPC_MAX_EVENTS];

    int nready = epoll_wait(srv->epoll_fd, events, IPC_MAX_EVENTS, timeout_ms);
    if (nready < 0)
    {
        if (errno == EINTR)
            return 0;       /* 被信号打断不算错误，回去看 g_running */
        perror("local_server: epoll_wait");
        return -1;
    }

    for (int i = 0; i < nready; i++)
    {
        int fd = events[i].data.fd;
        uint32_t rev = events[i].events;

        /* 新连接到来 */
        if (fd == srv->listen_fd)
        {
            handle_new_conn(srv);
            continue;
        }

        /* 找到这个 fd 对应的客户端槽位 */
        ipc_client_t *cli = NULL;
        for (int j = 0; j < IPC_MAX_CLIENTS; j++)
        {
            if (srv->clients[j].fd == fd)
            {
                cli = &srv->clients[j];
                break;
            }
        }

        if (!cli)
            continue;       /* 已经关掉的 fd，事件和槽位对不上，忽略 */

        /*
         * 先处理挂断再处理可读：
         * 对端关掉后 epoll 会同时报 EPOLLIN | EPOLLRDHUP，
         * 先 close 再 continue，避免对一个已经无效的 fd 做 recv。
         */
        if (rev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
        {
            close_client(srv, cli);
            continue;
        }
        if (rev & EPOLLIN)
        {
            handle_client_read(srv, cli);
        }
    }

    return 0;
}

static void handle_new_conn(local_server_t *srv)
{
    /*
     * accept4 = accept + 原子地设置 O_NONBLOCK / FD_CLOEXEC，
     * 比 accept 之后再 fcntl 少一次竞态（fork/exec 窗口）。
     */
    int connfd = accept4(srv->listen_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd < 0)
    {
        /* 监听 fd 是非阻塞的，队列空时返回 EAGAIN，属正常情况 */
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            perror("local_server: accept4");
        return;
    }

    /* 找空槽位；找不到就只拒绝这一个连接，不影响已有客户端 */
    ipc_client_t *cli = NULL;
    for (int j = 0; j < IPC_MAX_CLIENTS; j++)
    {
        if (srv->clients[j].fd < 0)
        {
            cli = &srv->clients[j];
            break;
        }
    }

    if (!cli)
    {
        close(connfd);
        return;
    }

    cli->fd       = connfd;
    cli->inlen    = 0;
    cli->is_local = true;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = connfd;
    ev.events  = EPOLLIN | EPOLLRDHUP;
    if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, connfd, &ev) < 0)
    {
        perror("local_server: epoll_ctl connfd");
        /*
         * 只回收这一个客户端槽位。
         * 原实现把 listen_fd / epoll_fd 置 -1，等于因为一个客户端
         * 加不进 epoll 就把整个本地服务废掉，而且 fd 没关会泄漏。
         */
        close(connfd);
        cli->fd = -1;
        return;
    }

    /* 连接建立先推一帧完整快照，Qt 首屏不用等下一个广播周期 */
    send_snapshot_to(srv, cli);
}

static void handle_client_read(local_server_t *srv, ipc_client_t *cli)
{
    /*
     * 缓冲区已经满且仍没有换行：要么对端不发换行，要么在灌垃圾。
     * 继续 recv 也没有地方放，直接断开。
     */
    if (cli->inlen >= sizeof(cli->inbuf))
    {
        close_client(srv, cli);
        return;
    }

    ssize_t r = recv(cli->fd,
                     cli->inbuf + cli->inlen,
                     sizeof(cli->inbuf) - cli->inlen,
                     0);
    if (r == 0)
    {
        close_client(srv, cli);     /* 对端正常关闭 */
        return;
    }
    if (r < 0)
    {
        /* 非阻塞 fd 读空、或被信号打断：都不是断开的理由 */
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        close_client(srv, cli);
        return;
    }

    cli->inlen += (size_t)r;

    /*
     * 字节流没有消息边界：一次 recv 可能读到半行，也可能读到好几行。
     * 先切出所有完整的行，剩下的残行用 memmove 挪到缓冲区开头。
     *
     * 空行直接跳过；非空行交给 handle_client_line() 解析。
     * 如果响应发送失败（对端已经不可写），立刻断开，不再处理后面的行。
     */
    size_t consumed = 0;
    for (size_t i = 0; i < cli->inlen; i++)
    {
        if (cli->inbuf[i] != '\n')
            continue;

        if (i > consumed &&
            handle_client_line(cli, cli->inbuf + consumed, i - consumed) < 0)
        {
            close_client(srv, cli);
            return;
        }
        consumed = i + 1;
    }

    if (consumed > 0)
    {
        memmove(cli->inbuf,
                cli->inbuf + consumed,
                cli->inlen - consumed);
        cli->inlen -= consumed;
    }

    /*
     * 切完行后缓冲区仍然是满的，说明对端发了一行超长数据且没有换行。
     * 不必等下一条 EPOLLIN，这里直接断开。
     */
    if (cli->inlen >= sizeof(cli->inbuf))
    {
        close_client(srv, cli);
    }
}

/*
 * 发一整帧。
 *
 * 返回值：
 *   0  —— 整帧发完；或者一个字节都没发出去就遇到 EAGAIN，这一帧丢了，
 *         但连接本身还是好的，下个周期再发。
 *  -1  —— 连接不可再用，调用方应 close_client()。
 *
 * 为什么发了一半 EAGAIN 不能"下帧再来"：
 *   send 是字节流，已经写出去那半行会和下一帧接在一起。对端按 '\n' 切行时
 *   会把两帧拼成一条解析不了的 JSON。要么为每个客户端维护发送队列，
 *   要么断开重连 —— 本地 UI 场景下断开更简单，代价可以接受。
 */
static int send_frame(ipc_client_t *cli, const char *line, size_t len)
{
    size_t off = 0;

    while (off < len)
    {
        ssize_t w = send(cli->fd, line + off, len - off, MSG_NOSIGNAL);
        if (w > 0)
        {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;

        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && off == 0)
        {
            return 0;       /* 一帧都没发出去：丢帧，保留连接 */
        }

        return -1;          /* 半帧已发出 / 真实错误：连接不能再用 */
    }

    return 0;
}

/*
 * 发一条命令响应。
 *
 * err 只接受固定字符串（"bad json" / "missing cmd" / "unknown cmd"），
 * 所以这里直接用 snprintf 拼，不需要 JSON 转义。以后如果要回显用户输入，
 * 必须换成 cJSON 或自己做转义。
 */
static int send_response(ipc_client_t *cli, double seq, int ok, const char *err)
{
    char resp[192];
    int n;

    if (ok)
    {
        n = snprintf(resp, sizeof(resp),
                     "{\"seq\":%.0f,\"ok\":true}\n", seq);
    }
    else
    {
        n = snprintf(resp, sizeof(resp),
                     "{\"seq\":%.0f,\"ok\":false,\"err\":\"%s\"}\n",
                     seq, err ? err : "error");
    }

    if (n < 0 || (size_t)n >= sizeof(resp))
        return -1;

    return send_frame(cli, resp, (size_t)n);
}

/*
 * 解析并执行一行命令。
 *
 * 返回 0：处理完（包括回了一条错误响应）
 * 返回 -1：响应发不出去，调用方应 close_client()
 *
 * 用 cJSON_ParseWithLength 而不是 cJSON_Parse：
 *   inbuf 里的行没有 '\0' 结尾，cJSON_Parse 会越界往后读；带长度的版本
 *   把解析范围钉死在这一行内。
 *
 * 目前只实现 ping，用来验证请求/响应链路。加新命令就在下面的 if/else
 * 里扩展，建议每个命令一个处理函数，不要把逻辑堆在这里。
 */
static int handle_client_line(ipc_client_t *cli, const char *line, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root)
        return send_response(cli, 0, 0, "bad json");

    double seq = 0;
    cJSON *jseq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    if (cJSON_IsNumber(jseq))
        seq = jseq->valuedouble;

    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(jcmd) || !jcmd->valuestring)
    {
        cJSON_Delete(root);
        return send_response(cli, seq, 0, "missing cmd");
    }

    int rc;
    if (strcmp(jcmd->valuestring, "ping") == 0)
    {
        rc = send_response(cli, seq, 1, NULL);
    }
    else
    {
        rc = send_response(cli, seq, 0, "unknown cmd");
    }

    cJSON_Delete(root);
    return rc;
}

static void send_snapshot_to(local_server_t *srv, ipc_client_t *cli)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t timestamp_ms = (uint64_t)ts.tv_sec * 1000
                            + (uint64_t)ts.tv_nsec / 1000000ULL;

    pthread_mutex_lock(&g_mpu_lock);
    mpu6050_data_t mpu = g_latest_mpu;
    pthread_mutex_unlock(&g_mpu_lock);

    pthread_mutex_lock(&g_ppg_lock);
    PPG_Result_t ppg = g_latest_ppg;
    pthread_mutex_unlock(&g_ppg_lock);

    char *json = build_snapshot_json(timestamp_ms, &mpu, &ppg);
    if (!json)
    {
        /* 内存不足才会走到这里，跳过这一帧即可，不配影响连接 */
        return;
    }

    /*
     * 只发给刚连上的这个客户端。
     * build_snapshot_json() 返回的字符串末尾已经带 '\n'，这里原样发出，
     * 不要再补换行，否则会变成空行 + 帧。
     */
    if (send_frame(cli, json, strlen(json)) < 0)
    {
        close_client(srv, cli);
    }
    free(json);
}

void local_server_broadcast_snapshot(local_server_t *srv, const char *json_line)
{
    /* 约定：json_line 必须是一条完整的 NDJSON 行（末尾带 '\n'） */
    size_t len = strlen(json_line);
    if (len == 0)
        return;

    for (int j = 0; j < IPC_MAX_CLIENTS; j++)
    {
        ipc_client_t *cli = &srv->clients[j];
        if (cli->fd < 0)
            continue;

        if (send_frame(cli, json_line, len) < 0)
        {
            close_client(srv, cli);
        }
    }
}

static void close_client(local_server_t *srv, ipc_client_t *cli)
{
    /*
     * 幂等：广播失败和 EPOLLHUP 可能先后都调到同一个槽位，
     * 第二次进来时 fd 已经是 -1，直接返回，避免 close(-1) 和重复 EPOLL_CTL_DEL。
     */
    if (cli->fd < 0)
        return;

    epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, cli->fd, NULL);
    close(cli->fd);
    cli->fd    = -1;
    cli->inlen = 0;
}

void local_server_destroy(local_server_t *srv)
{
    if (!srv)
        return;

    for (int j = 0; j < IPC_MAX_CLIENTS; j++)
    {
        if (srv->clients[j].fd >= 0)
        {
            close_client(srv, &srv->clients[j]);
        }
    }

    if (srv->epoll_fd >= 0)
        close(srv->epoll_fd);
    if (srv->listen_fd >= 0)
        close(srv->listen_fd);
    srv->epoll_fd = srv->listen_fd = -1;

    if (srv->sock_path[0] != '\0')
        unlink(srv->sock_path);
}
