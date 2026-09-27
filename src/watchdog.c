/**
 * @file    watchdog.c
 * @brief   硬件看门狗 + 复位计数 —— 无人值守运行的最后一道防线
 *
 * ═══════════════════════════════════════════════════════════════════
 *  一、它解决什么问题
 * ═══════════════════════════════════════════════════════════════════
 *
 * 前面几层"能退出"的手段（协作式退出、带超时的 join、_exit 兜底）
 * 有一个共同前提：**进程自己还跑得起来**。
 *
 * 一旦某个线程陷进内核出不来（典型是 TASK_UNINTERRUPTIBLE / D 状态），
 * 信号根本投递不进那个线程；主线程的 pthread_timedjoin_np 超时也只是
 * "我不等了"，那个线程本身还挂在那儿 —— 进程的地址空间回收不掉，
 * Ctrl+C、Ctrl+Z 全部石沉大海，最后只能拔电。
 *
 * 硬件看门狗是唯一不依赖进程状态的手段：SoC 内部一个独立计数器，
 * 走到底直接拉复位线，进程处于什么状态都拦不住它。
 *
 * ═══════════════════════════════════════════════════════════════════
 *  二、硬性约束：WATCHDOG_TIMEOUT_S 必须 <= 128
 * ═══════════════════════════════════════════════════════════════════
 *
 * 本板的看门狗驱动是 imx2_wdt，probe 里设了
 *     wdog->max_hw_heartbeat_ms = IMX2_WDT_MAX_TIME * 1000;   // 128000
 *
 * 而 watchdog_dev.c 里有一段"内核替用户态补喂"的逻辑：
 *
 *     static inline bool watchdog_need_worker(struct watchdog_device *wdd)
 *     {
 *         unsigned int hm = wdd->max_hw_heartbeat_ms;
 *         unsigned int t  = wdd->timeout * 1000;
 *
 *         return (hm && watchdog_active(wdd) && t > hm) ||
 *                (t && !watchdog_active(wdd) && watchdog_hw_running(wdd));
 *     }
 *
 * 第一个条件的意思是：**用户态要求的超时比硬件能给的还长时，内核用
 * hrtimer 替用户态补喂**。所以只要 WATCHDOG_TIMEOUT_S 一超过 128，
 * 哪怕本进程整个卡死在内核里，内核也会一直替它喂 —— 看门狗彻底失效。
 * 这条线不能越，下面用 #error 在编译期钉死。
 *
 * ═══════════════════════════════════════════════════════════════════
 *  三、为什么退出时不需要"把看门狗关掉"（和大多数教程不一样）
 * ═══════════════════════════════════════════════════════════════════
 *
 * imx2+ 的看门狗**硬件上就停不了** —— imx2_wdt 的 watchdog_ops 里
 * 根本没有 .stop 这一项（可以去看 drivers/watchdog/imx2_wdt.c:239）。
 * 所以"magic close 关掉看门狗"在本板上是不成立的。
 *
 * 那为什么正常退出后板子不会被喂重启？因为 watchdog_dev.c 的
 * watchdog_stop() 在驱动没有 .stop 时会走 else 分支：
 *
 *     if (wdd->ops->stop) { ... err = wdd->ops->stop(wdd); }
 *     else                { set_bit(WDOG_HW_RUNNING, &wdd->status); }
 *
 * 硬件计数器照跑，但 watchdog_release() 随后会清掉 WDOG_ACTIVE 并调用
 * watchdog_update_worker()，此时 watchdog_need_worker() 的第二个条件
 *     (t && !watchdog_active(wdd) && watchdog_hw_running(wdd))
 * 恰好为真 —— **从这一刻起由内核接管喂狗**。
 *
 * 这正是 CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED 的语义："没人打开看门狗
 * 设备时，内核负责喂它。" 于是形成一条很漂亮的分工：
 *
 *   进程还活着            → 进程喂（内核不插手，因为 t <= hm，第一个条件
 *                            不成立 → 内核的 hrtimer 是停着的）
 *   进程正常退出          → 内核接手喂（板子不会被误复位）
 *   进程活着但整体卡死    → 没人喂（fd 没关，WDOG_ACTIVE 还在，内核既不满足
 *                            第一个条件也不满足第二个）→ 30 秒后硬件复位 ✓
 *
 * 第三条就是我们要的效果，而且是"没人关 fd"这个物理事实自然带来的，
 * 不需要任何额外机制。
 */

#include "globals.h"    /* 必须在系统头文件之前——引入特性宏；同时带来 g_running / tcp_log */
#include "config.h"
#include "watchdog.h"

#ifdef WATCHDOG_ENABLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/watchdog.h>

/* imx2_wdt 的硬件超时上限（WCR 的 WT 域 8 bit，(s*2-1)<<8 编码 → 最大 128 秒） */
#define IMX2_WDT_MAX_TIMEOUT_S  128

#if WATCHDOG_TIMEOUT_S > IMX2_WDT_MAX_TIMEOUT_S
#error "WATCHDOG_TIMEOUT_S 超过 128：内核会接替用户态补喂看门狗，保护完全失效。见 watchdog.c 顶部说明。"
#endif

/* ═══ 各线程心跳 ═══ */
/*
 * 每个槽位只有一个线程写（wd_beat 里是 wd_beats[ch]++），
 * unsigned int 在 ARM 上是字长对齐的，编译成单条 LDR/STR ——
 * 单写单读不会读到"半个值"。这是这里敢不上原子类型或锁的前提。
 * 换成 uint64_t 就不成立了：32 位 ARM 上要两条指令，会撕裂。
 */
static volatile unsigned int wd_beats[WD_CH_COUNT];

static const char *const wd_ch_name[WD_CH_COUNT] = {
    "max30102", "mpu6050", "display", "tcp", "http",
};

/* ═══ 看门狗设备句柄 ═══ */
static int wd_fd = -1;

/* 本次启动时从硬件复位状态寄存器读到的复位原因标志 */
static int wd_bootstatus;

/* ═══ 跨重启统计 ═══ */
typedef struct {
    unsigned int boots_total;    /* 累计启动次数 */
    unsigned int unclean_boots;  /* 其中"上次没走完正常收尾"的次数 */
    unsigned int wd_resets;      /* 硬件登记为看门狗超时的次数 */
    int          last_clean;     /* 上次退出有没有写 CLEAN 标记（1=干净） */
} wd_stats_t;

static wd_stats_t wd_stats;


/* ═══════════════════════════════════════════════════════════════════
 *  统计文件位置解析
 *
 *  配置里只写死了子目录名，绝对路径在运行时拼 —— 因为这个程序会被
 *  两种身份运行，家目录不一样：
 *
 *    普通用户  ./build/health_monitor          $HOME = /home/debian
 *    sudo 运行  sudo ./build/health_monitor     $HOME = /root （被 sudo 重置了）
 *
 *  sudo 会把 HOME 改掉，但**同时**会设一个 SUDO_USER 环境变量记下
 *  "是谁在提权"。所以先看 SUDO_USER，能拿到就查它的 passwd 记录要家目录 ——
 *  这样加不加 sudo，统计文件都落在同一个地方，不会莫名其妙分裂成两份。
 *
 *  为什么不用 getpwuid(getuid())：sudo 之后 getuid() 返回 0，查到的是 root
 *  的家目录，和 HOME 是同一个问题。SUDO_USER 是唯一能还原"原始用户"的线索。
 *
 *  兜底顺序的最后一级是当前工作目录。HOME 在某些精简环境（systemd 服务
 *  没写 User= 以外的家目录、容器里）可能是空的，那时至少还能落在 cwd。
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * 目录名最长 PATH_MAX，文件名在其后拼接，所以给后者多留一截余量 ——
 * 否则 GCC 的 -Wformat-truncation 会警告"可能截断"，而且它是对的：
 * 家目录接近 PATH_MAX 时拼出来的文件名真的会被截掉尾巴。
 */
#define WD_FILE_PATH_MAX  (PATH_MAX + 64)

static char wd_state_dir[PATH_MAX];
static char wd_state_file[WD_FILE_PATH_MAX];
static char wd_log_file[WD_FILE_PATH_MAX];

/*
 * 取"真实用户"的家目录。
 *
 * 优先 SUDO_USER（提权场景），其次 $HOME（普通场景），都没有就返回 NULL
 * 让调用方退化到 cwd。返回值是传进来的缓冲区，失败时为 NULL。
 */
static const char *wd_home_dir(char *buf, size_t buflen)
{
    const char *sudo_user = getenv("SUDO_USER");

    if (sudo_user != NULL && sudo_user[0] != '\0') {
        struct passwd *pw = getpwnam(sudo_user);   /* 非线程安全，但这里只在启动时单线程调用 */
        if (pw != NULL && pw->pw_dir != NULL && pw->pw_dir[0] != '\0') {
            snprintf(buf, buflen, "%s", pw->pw_dir);
            return buf;
        }
    }

    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        snprintf(buf, buflen, "%s", home);
        return buf;
    }

    return NULL;
}

/*
 * 解析出三个绝对路径。只调用一次（wd_init 开头），解析结果全局复用。
 * 失败也照样把路径填好（退化成 cwd 下），后续 open 失败会自然被记录。
 */
static void wd_resolve_state_dir(void)
{
    const char *env = getenv(WATCHDOG_STATE_ENV);
    if (env != NULL && env[0] != '\0') {
        snprintf(wd_state_dir, sizeof(wd_state_dir), "%s", env);
    } else {
        /*
         * 这里缓冲区比 PATH_MAX 少几个字节，是为了给下面拼接的 "/demo"
         * 留位置。不留的话，家目录长度接近 PATH_MAX 时结果会被截断成一个
         * 残缺路径（不会报错，只会静默落到别的地方去）。
         */
        char home[PATH_MAX - 8];
        if (wd_home_dir(home, sizeof(home)) != NULL)
            snprintf(wd_state_dir, sizeof(wd_state_dir),
                     "%s/%s", home, WATCHDOG_STATE_SUBDIR);
        else
            snprintf(wd_state_dir, sizeof(wd_state_dir),
                     "./%s", WATCHDOG_STATE_SUBDIR);
    }

    /* 去掉末尾斜杠，否则拼出来会变成 "dir//file"（能用，但难看） */
    size_t n = strlen(wd_state_dir);
    while (n > 1 && wd_state_dir[n - 1] == '/')
        wd_state_dir[--n] = '\0';

    snprintf(wd_state_file, sizeof(wd_state_file),
             "%s/%s", wd_state_dir, WATCHDOG_STATE_FILE_NAME);
    snprintf(wd_log_file, sizeof(wd_log_file),
             "%s/%s", wd_state_dir, WATCHDOG_LOG_FILE_NAME);
}

/*
 * 在 sudo 下运行时进程是 root，建出来的目录和文件都归 root。之后如果不带
 * sudo 再跑一次，就会因为写不进去而**静默**丢掉统计 —— 而"统计能不能持久化"
 * 恰恰是这个功能存在的意义（统计断了，就分不清"系统很稳"和"根本没在看"）。
 *
 * sudo 会留下 SUDO_UID / SUDO_GID 两个环境变量记录原始用户，有就顺手把属主
 * 改回去。这样加不加 sudo 跑，统计文件都能继续往下写。
 *
 * 只在 getuid()==0（确实是 root）时才动手：普通用户身份下 chown 必然失败，
 * 白白刷一行错误。
 */
static void wd_restore_owner(const char *path)
{
    const char *uid_s = getenv("SUDO_UID");
    const char *gid_s = getenv("SUDO_GID");
    if (uid_s == NULL || gid_s == NULL || getuid() != 0)
        return;

    char *end;
    long uid = strtol(uid_s, &end, 10);
    if (*end != '\0' || uid < 0)
        return;
    long gid = strtol(gid_s, &end, 10);
    if (*end != '\0' || gid < 0)
        return;

    if (chown(path, (uid_t)uid, (gid_t)gid) < 0)
        fprintf(stderr, "[WATCHDOG] chown %s 失败: %s\n", path, strerror(errno));
}


/* ═══════════════════════════════════════════════════════════════════
 *  统计文件读写
 *
 *  格式是最朴素的 key=value 文本，一行一个。
 *  不用二进制结构体：结构体布局随编译器、对齐、字段增删变化，而这个
 *  小文件是要跨"程序重新编译"甚至跨版本读的；文本格式改字段不会读错，
 *  最多是新字段缺省。
 *
 *  「上次干净退出」这个标记是整套计数里最可靠的一环 —— 它不依赖任何
 *  硬件语义，只看"程序有没有走到最后一行"。而 WDIOC_GETBOOTSTATUS
 *  读到的 WRSR_TOUT 位是否被硬件在下次复位时清掉，要实测才知道
 *  （imx2_wdt 只读不清，见 imx2_wdt.c:304）。两个一起记，互为佐证。
 * ═══════════════════════════════════════════════════════════════════ */

static void wd_load_stats(void)
{
    FILE *fp = fopen(wd_state_file, "r");
    if (fp == NULL)
        return;     /* 文件不存在（首次运行）或不可读：保持调用方给的默认值 */

    char line[64];
    unsigned int v;

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "boots_total=%u", &v) == 1)
            wd_stats.boots_total = v;
        else if (sscanf(line, "unclean_boots=%u", &v) == 1)
            wd_stats.unclean_boots = v;
        else if (sscanf(line, "wd_resets=%u", &v) == 1)
            wd_stats.wd_resets = v;
        else if (sscanf(line, "last_clean=%u", &v) == 1)
            wd_stats.last_clean = (int)v;
    }
    fclose(fp);
}

/*
 * 为什么是"写临时文件 → fsync → rename"，而不是直接覆盖：
 *
 *   直接覆盖是"先截断再写"，写到一半被复位，文件就剩半截，下次读到的
 *   是垃圾。rename 在同一文件系统内是原子操作 —— 读到的要么是完整的
 *   旧内容，要么是完整的新内容，不存在中间态。
 *
 *   fsync 也不能省。ext4 默认延迟分配，rename 完数据块可能还在页缓存里。
 *   看门狗复位是 SoC 内部热复位（不掉电），页缓存不会丢，所以那种情况
 *   侥幸没事；但用户直接拔电就是真丢。一次 fsync 换"两种复位都不丢"。
 */
static void wd_save_stats(void)
{
    char tmp_path[WD_FILE_PATH_MAX + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", wd_state_file);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        /*
         * 这里必须出声。统计写不进去有两种典型原因：目录属主不对（混用了
         * sudo 和普通身份）、rootfs 只读。静默返回会让用户以为"一直没重启"，
         * 实际上只是从来没记上 —— 那是最难查的一类问题。
         * 本函数每次运行只调两次（init 和 stop），不会刷屏。
         */
        fprintf(stderr, "[WATCHDOG] 写入 %s 失败: %s（统计不会持久化）\n",
                tmp_path, strerror(errno));
        return;
    }

    char buf[192];
    int n = snprintf(buf, sizeof(buf),
                     "boots_total=%u\n"
                     "unclean_boots=%u\n"
                     "wd_resets=%u\n"
                     "last_clean=%d\n",
                     wd_stats.boots_total, wd_stats.unclean_boots,
                     wd_stats.wd_resets, wd_stats.last_clean);

    if (n > 0) {
        ssize_t w = write(fd, buf, (size_t)n);
        (void)w;
    }
    fsync(fd);
    close(fd);

    /* 改属主要在 rename 之前：rename 之后这个 inode 就是正式文件了 */
    wd_restore_owner(tmp_path);

    if (rename(tmp_path, wd_state_file) < 0)
        unlink(tmp_path);   /* rename 失败（跨文件系统等），别留垃圾 */
}

/*
 * 每次启动往日志追加一行。只增不改，用户可以
 *     tail -n 30 ~/demo/watchdog_boots.log
 * 直接看这几天稳不稳，不用去翻 dmesg 或数屏幕上的 tcp_log。
 *
 * 时间戳用 CLOCK_REALTIME。板子没有带电池的 RTC 时，开机时间从
 * 镜像的构建时间起算，NTP 对时前这个时间戳是错的 —— 看的时候留意。
 */
static void wd_append_boot_log(const char *verdict, int armed)
{
    FILE *fp = fopen(wd_log_file, "a");
    if (fp == NULL)
        return;

    wd_restore_owner(wd_log_file);

    time_t now = time(NULL);
    struct tm tmv;
    char ts[32] = "时间未知";

    if (localtime_r(&now, &tmv) != NULL)
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    /*
     * 每条都标出"这次有没有看门狗"。
     * 没有这行标记的话，一段"没有看门狗复位"的记录会被误读成
     * "系统很稳" —— 其实只是压根没人看着。
     */
    fprintf(fp, "%s  %s  (第 %u 次启动, 异常退出 %u 次, 看门狗复位 %u 次, %s)\n",
            ts, verdict, wd_stats.boots_total,
            wd_stats.unclean_boots, wd_stats.wd_resets,
            armed ? "看门狗已接管" : "无看门狗");
    fclose(fp);
}


/* ═══════════════════════════════════════════════════════════════════
 *  看门狗操作
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * 喂狗有两条等价路径：
 *   write(fd, 任意字节, 1)      → watchdog_write() 里调 watchdog_ping()
 *   ioctl(fd, WDIOC_KEEPALIVE)  → watchdog_ioctl() 里调 watchdog_ping()
 * 最终都落到 imx2_wdt_ping()：向 WSR 寄存器**按顺序**写 0x5555 再写 0xAAAA
 * （顺序写错不算数，这是硬件防误喂的设计）。用 ioctl 是因为语义更直白，
 * 不受"写了几个字节"的影响。
 *
 * 失败只打一行 stderr，不做别的：喂狗失败意味着保护正在失效，但这时候
 * 能做的补救（重开设备）比失败本身更危险。留给用户看日志。
 */
static void wd_feed(void)
{
    if (wd_fd < 0)
        return;
    if (ioctl(wd_fd, WDIOC_KEEPALIVE, 0) < 0)
        fprintf(stderr, "[WATCHDOG] 喂狗失败: %s\n", strerror(errno));
}


int wd_init(void)
{
    /* ═══ ① 先定下统计文件的位置 ═══ */
    /*
     * 这一步刻意放在开设备之前：跨重启记账和硬件看门狗是两件独立的事。
     * 打不开 /dev/watchdog 时照样要记"这次启动过" —— 否则一旦忘了用 sudo，
     * 统计就凭空断档，反而看不出程序到底重启了几次。
     */
    wd_resolve_state_dir();

    if (mkdir(wd_state_dir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "[WATCHDOG] 无法创建统计目录 %s: %s（统计不会持久化）\n",
                wd_state_dir, strerror(errno));
    } else {
        wd_restore_owner(wd_state_dir);
        fprintf(stderr, "[WATCHDOG] 统计目录: %s\n", wd_state_dir);
    }

    /* ═══ ② 尝试接管硬件看门狗（失败就降级，不影响下面的记账）═══ */
    int t = 0;   /* 实际生效的超时秒数，0 表示没拿到 */

    if (access(WATCHDOG_DEV, F_OK) < 0) {
        fprintf(stderr,
                "[WATCHDOG] %s 不存在 —— 本次运行没有看门狗保护\n"
                "  需要内核打开 CONFIG_IMX2_WDT，且设备树里 wdog1 为 okay\n",
                WATCHDOG_DEV);
    } else {
        /*
         * O_WRONLY：看门狗设备只写（读会返回 -EINVAL）。
         *
         * 绝对不要加 O_NONBLOCK。非阻塞打开会让 watchdog_open() 跳过
         * "等待设备可用"的分支，语义上不再是"独占并接管"。
         *
         * 注意：open() 这一步本身就已经把硬件看门狗启动了（内核在里面调了
         * driver 的 .start），此刻超时是驱动默认值 IMX2_WDT_DEFAULT_TIME
         * = 60 秒。所以下面必须紧接着把超时改成我们要的值，中间别插耗时操作。
         */
        wd_fd = open(WATCHDOG_DEV, O_WRONLY | O_CLOEXEC);

        if (wd_fd < 0) {
            fprintf(stderr,
                "\n"
                "[WATCHDOG] 打开 %s 失败: %s\n"
                "  >>> 本次运行【没有看门狗保护】：进程卡死时板子不会自动复位 <<<\n"
                "  该设备默认 0600 root:root，普通用户打不开。做法：\n"
                "    sudo ./build/health_monitor\n"
                "    sudo HEALTH_MONITOR_STATE_DIR=$HOME/demo ./build/health_monitor\n"
                "  第二条里的环境变量不能省 —— sudo 会把 HOME 重置成 /root，\n"
                "  不显式指定的话统计文件会悄悄落到 /root/demo 去。\n"
                "  心跳停滞检测照常工作，只是检测到之后只能打印告警。\n"
                "\n",
                WATCHDOG_DEV, strerror(errno));
        } else {
            /* 取驱动能力信息，日志里留个底（imx2_wdt 的 identity 是 "imx2+ watchdog"）*/
            struct watchdog_info info;
            if (ioctl(wd_fd, WDIOC_GETSUPPORT, &info) == 0) {
                fprintf(stderr, "[WATCHDOG] 设备: %s, 能力位: 0x%08x\n",
                        info.identity, info.options);
            }

            /*
             * 设置超时。内核会把"实际生效的值"回填到同一个变量 ——
             * imx2_wdt 的合法范围是 1~128 秒，超范围会被夹到边界，
             * 所以必须读回来看。
             */
            t = WATCHDOG_TIMEOUT_S;
            if (ioctl(wd_fd, WDIOC_SETTIMEOUT, &t) < 0) {
                fprintf(stderr, "[WATCHDOG] WDIOC_SETTIMEOUT 失败: %s\n",
                        strerror(errno));
                /*
                 * 设不上就退回驱动当前的超时值再报，别拿 0 糊弄 ——
                 * 报 0 会让人以为看门狗没在工作，而它其实正用默认值
                 * （imx2_wdt 是 60 秒）跑着。
                 */
                t = 0;
                int cur = 0;
                if (ioctl(wd_fd, WDIOC_GETTIMEOUT, &cur) == 0)
                    t = cur;
            } else if (t > IMX2_WDT_MAX_TIMEOUT_S) {
                fprintf(stderr, "[WATCHDOG] 实际超时 %d 秒 > %d 秒，"
                                "内核会自己补喂，看门狗失去意义\n",
                        t, IMX2_WDT_MAX_TIMEOUT_S);
            }

            /* 读硬件复位状态寄存器：上一次复位是不是看门狗干的 */
            int st = 0;
            if (ioctl(wd_fd, WDIOC_GETBOOTSTATUS, &st) == 0)
                wd_bootstatus = st;

            wd_feed();  /* 立刻喂一次，把 open 到现在的窗口收干净 */
        }
    }

    /* 后面判断/打印统一用这个：它精确对应"我们能不能停止喂狗" */
    const int armed = (wd_fd >= 0);

    /* ═══ ③ 跨重启记账（与硬件是否接管无关，每次都记）═══ */
    memset(&wd_stats, 0, sizeof(wd_stats));
    wd_stats.last_clean = 1;   /* 首次运行（文件不存在）视为"上次是干净的" */
    wd_load_stats();

    int prev_clean = wd_stats.last_clean;

    wd_stats.boots_total++;
    if (!prev_clean)
        wd_stats.unclean_boots++;
    if (wd_bootstatus & WDIOF_CARDRESET)
        wd_stats.wd_resets++;

    wd_stats.last_clean = 0;   /* 本次运行开始，"脏"标记就位 */
    wd_save_stats();

    const char *verdict;
    if (wd_bootstatus & WDIOF_CARDRESET)
        verdict = "上次由看门狗超时复位";
    else if (!prev_clean)
        verdict = "上次非正常退出（没走完收尾流程）";
    else
        verdict = "上次正常退出";

    fprintf(stderr, "[WATCHDOG] 第 %u 次启动 | %s\n",
            wd_stats.boots_total, verdict);
    fprintf(stderr, "[WATCHDOG] 累计: 异常退出 %u 次, 看门狗复位 %u 次\n",
            wd_stats.unclean_boots, wd_stats.wd_resets);

    if (armed)
        fprintf(stderr,
                "[WATCHDOG] 保护已生效: 超时 %d 秒 | 线程停滞 >%d 秒即停止喂狗\n",
                t, WATCHDOG_STALE_MS / 1000);
    else
        fprintf(stderr,
                "[WATCHDOG] !!! 未接管硬件看门狗，本次运行只有告警能力 !!!\n");

    wd_append_boot_log(verdict, armed);

    return armed ? 0 : -1;
}


void wd_beat(int ch)
{
    if (ch >= 0 && ch < WD_CH_COUNT)
        wd_beats[ch]++;
}


void wd_stop(void)
{
    if (wd_fd < 0)
        return;

    /*
     * magic close：先写一个 'V'，再 close。
     *
     * watchdog_dev.c 的 watchdog_release() 里：
     *     else if (test_and_clear_bit(_WDOG_ALLOW_RELEASE, &wd_data->status) ||
     *              !(wdd->info->options & WDIOF_MAGICCLOSE))
     *         err = watchdog_stop(wdd);
     * _WDOG_ALLOW_RELEASE 就是被那个 'V' 置上的。
     *
     * imx2_wdt 没有 .stop，所以 watchdog_stop() 只是置上 WDOG_HW_RUNNING、
     * 清掉 WDOG_ACTIVE，硬件计数器照跑；紧接着 release 里的
     * watchdog_update_worker() 会因为 need_worker 的第二个条件成立而启动
     * 内核的补喂定时器 —— **从这一刻起由内核替我们喂狗**。
     * 所以正常退出不会把板子喂重启。详见本文件顶部的第三节。
     *
     * 本内核 CONFIG_WATCHDOG_NOWAYOUT 没打开，所以 magic close 是被允许的。
     */
    ssize_t n = write(wd_fd, "V", 1);
    (void)n;
    close(wd_fd);
    wd_fd = -1;

    wd_stats.last_clean = 1;   /* 告诉下次启动："这次是走完收尾才退的" */
    wd_save_stats();
}


/* ═══════════════════════════════════════════════════════════════════
 *  巡检线程
 *
 *  本线程只干一件事：定期看一眼各线程的心跳有没有在动，动就喂狗，
 *  不动就停喂。它一旦自己卡死，自然就没人喂了 —— 自我监视是天然的，
 *  不需要额外机制。
 *
 *  ── 两个必须说清楚的实现细节 ──
 *
 *  ① 停滞判定按**真实流逝时间**算，不按"巡检了多少轮"。
 *
 *     按轮数算是错的，而且错过一次就很难查：轮数只有在 sleep 真的睡了
 *     那么久时才等于时间。sleep 一旦因为任何原因提前返回（EINTR、参数
 *     非法、被抢占），轮数就在微秒级冲到阈值，把一批正常的线程全判死，
 *     然后驱动去复位板子 —— 一个本该保护系统的机制反过来成了故障源。
 *     所以这里每轮都用 get_sys_ms()（CLOCK_MONOTONIC 毫秒）算真实间隔。
 *
 *  ② 睡眠用 clock_nanosleep + TIMER_ABSTIME，不用 nanosleep。
 *
 *     三个理由：
 *       - nanosleep 的 tv_nsec 必须 < 1e9，把毫秒直接乘 1e6 塞进去
 *         （2000ms → 2e9）会被判 EINVAL 并且**一纳秒都不睡**，返回值还
 *         容易被当成"被打断了"而忽略 —— 整个线程就成了满速自旋。
 *       - 相对时间睡眠会累积漂移（每次睡 2 秒 + 处理耗时，实际周期 >2 秒），
 *         绝对时间（下次唤醒时刻 = 上次唤醒时刻 + 周期）不会。
 *       - TIMER_ABSTIME 下被信号打断后重新调用会自动补齐剩余时间，
 *         不用自己处理 rem 参数。选 CLOCK_MONOTONIC 是因为本项目的
 *         MQTT 云端模式会开 NTP 对时，CLOCK_REALTIME 会跳变。
 * ═══════════════════════════════════════════════════════════════════ */
void *thread_watchdog(void *arg)
{
    (void)arg;

    const int armed = (wd_fd >= 0);   /* 没接管硬件看门狗时，只能告警 */

    unsigned int prev[WD_CH_COUNT];      /* 各通道上轮看到的心跳计数 */
    uint64_t     prev_ms[WD_CH_COUNT];   /* 各通道心跳最后一次变化的时间 */
    int          healthy = 1;
    int          i;

    uint64_t next_ms = get_sys_ms();

    for (i = 0; i < WD_CH_COUNT; i++) {
        prev[i]    = wd_beats[i];
        prev_ms[i] = next_ms;
    }

    while (g_running) {
        /* ── 睡到下一个巡检点（绝对时间，不累积漂移）── */
        next_ms += WATCHDOG_CHECK_INTERVAL_MS;

        struct timespec ts;
        ts.tv_sec  = (time_t)(next_ms / 1000u);
        ts.tv_nsec = (long)(next_ms % 1000u) * 1000000L;   /* 恒 < 1e9 */

        int rc;
        while ((rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                     &ts, NULL)) == EINTR)
            ;   /* 只是被信号打断：绝对时间没变，重新调一次即可 */

        if (rc != 0) {
            /*
             * clock_nanosleep 失败时直接返回错误码，不设 errno。
             * 绝不能忽略返回值继续循环 —— 那就是上面注释①说的自旋。
             * 宁可退化成粗粒度 sleep，也不能变成忙等。
             */
            fprintf(stderr, "[WATCHDOG] clock_nanosleep 失败(%s)，降级为 1 秒轮询\n",
                    strerror(rc));
            sleep(1);
            next_ms = get_sys_ms();
            continue;
        }

        /* ── 扫描各通道，找出停滞最久的那个 ── */
        uint64_t now = get_sys_ms();
        int      bad = -1;
        uint64_t worst = 0;

        for (i = 0; i < WD_CH_COUNT; i++) {
            if (wd_beats[i] != prev[i]) {
                prev[i]    = wd_beats[i];
                prev_ms[i] = now;          /* 有心跳：刷新它的时间戳 */
                continue;
            }
            uint64_t stuck = now - prev_ms[i];
            if (stuck >= WATCHDOG_STALE_MS && stuck > worst) {
                worst = stuck;             /* 报最久的那个，信息量最大 */
                bad   = i;
            }
        }

        if (bad >= 0) {
            if (healthy) {
                healthy = 0;
                unsigned long long stall_s = (unsigned long long)(worst / 1000u);

                if (armed) {
                    /*
                     * 停喂 —— 让硬件计数器自己走到零。
                     * 这里不 return、也不 _exit：万一那个线程只是慢（还没死透），
                     * 心跳恢复后下一轮会接着喂，板子就不用白重启一次。
                     */
                    tcp_log("[WATCHDOG] 线程 %s 停滞 %llu 秒，停止喂狗，%d 秒后复位",
                            wd_ch_name[bad], stall_s, WATCHDOG_TIMEOUT_S);
                    fprintf(stderr,
                            "[WATCHDOG] 线程 %s 已停滞 %llu 秒 → 停止喂狗，"
                            "%d 秒后整板复位\n",
                            wd_ch_name[bad], stall_s, WATCHDOG_TIMEOUT_S);
                } else {
                    /* 措辞必须和 armed 分支区分开：这时候说"即将复位"是撒谎 */
                    tcp_log("[WATCHDOG] 线程 %s 停滞 %llu 秒（无看门狗，仅告警）",
                            wd_ch_name[bad], stall_s);
                    fprintf(stderr,
                            "[WATCHDOG] 线程 %s 已停滞 %llu 秒（无看门狗，仅告警）\n",
                            wd_ch_name[bad], stall_s);
                }
            }
            continue;   /* 判死状态下不喂，让计数器照原样走到底 */
        }

        if (!healthy) {
            healthy = 1;
            tcp_log("[WATCHDOG] 线程心跳恢复，继续喂狗");
        }
        wd_feed();
    }

    /*
     * g_running 被信号处理函数置 0，进入收尾阶段 —— 本线程可以退出了，
     * 否则 main() 里的带超时 join 收不掉它。
     *
     * 为什么这里要喂最后一次：
     *   收尾阶段各功能线程本来就在陆续停跳，继续做"心跳停滞"判定只会误报，
     *   所以本线程直接收工；之后的保底交给 main() 自己 —— 收尾全程是有界的
     *   （7 次带超时的 join 各最多 2 秒 + 几个不阻塞的清理调用），远小于
     *   WATCHDOG_TIMEOUT_S，不会把板子喂重启。
     *
     * healthy 才喂：如果已经有线程被判死，这一喂等于白白把复位推迟一整个
     * 超时周期。判死状态下让计数器照原样走到底。
     */
    if (healthy)
        wd_feed();

    return NULL;
}

#else  /* !WATCHDOG_ENABLE —— 关掉时全部退化成空操作，调用点不用改 */

int wd_init(void) { return -1; }
void wd_beat(int ch) { (void)ch; }
void wd_stop(void) { }
void *thread_watchdog(void *arg) { (void)arg; return NULL; }

#endif /* WATCHDOG_ENABLE */
