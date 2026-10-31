/*
 * rrs_daemon_adaptive.c
 * 全局动态高刷 daemon（自动适配版）
 *
 * 与初版 RRS_Daemon 的区别：
 *   1) 刷新率档位不写死：启动时解析 `cmd display get-displays` 的
 *      sfModeId/fps 表，运行时按本机实际模式选档；
 *   2) 上限自动取：优先 device_features 的 support_max_fps（可选，
 *      默认取本机最高模式），智能模式自动选 top/mid(<=120)/low(<=60)；
 *   3) 触摸设备自动识别：解析 /proc/bus/input/devices，用 EVIOCGBIT
 *      验证 EV_ABS + ABS_MT_POSITION_X，优先真正的多点触摸屏；
 *   4) 触摸采样率默认不碰（本机系统已按模式映射），只有配置开启且
 *      回读验证生效时才写；
 *   5) 刷新率下发失败会回读验证，并回退到 settings/DisplayManager；
 *   6) 退出时恢复原刷新率与改动过的 settings。
 *
 * 构建：zig cc -target aarch64-linux-musl -static -O2 -pthread
 * 配置：/data/adb/RRS/config   （键=值，见 load_config）
 * 日志：/data/adb/RRS/rrs.log
 */

#define _GNU_SOURCE
#define MAX_APPS 512
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH      "/data/adb/RRS/rrs.log"
#define CONFIG_PATH   "/data/adb/RRS/config"
#define MAX_MODES     32
#define MAX_IDEV      32

typedef struct {
    int fps;
    int sf;       /* SurfaceFlinger sfModeId（1035 的参数） */
    int fw;       /* framework Display.Mode id */
    int w, h;
} Mode;

static Mode g_modes[MAX_MODES];
static int g_nmodes;
static int g_display_id = 0;
static int g_sf_auto_offset = -1;   /* Xiaomi: fw-1；ColorOS: fw */
static int g_top = -1, g_mid = -1, g_low = -1;
static int g_tiers[8];
static int g_ntiers;
static int g_delays[8] = { 1000, 2000, 2000, 2000, 2000, 2000, 2000, 2000 };
static int g_support_cap = 0;
static int g_cur_fps = -1;
static int g_orig_sf = -1;

static volatile int g_stop;
static int g_user = 0;
static int g_inotify_fd = -1;
static int g_wake_wr = -1;   /* fg_thread 唤醒主循环的管道写端 */
static pid_t g_logcat_pid = -1;
static int g_touch_fd = -1;
static char g_touch_path[64];

static pthread_mutex_t g_log_lock;
static int g_log_fd = -1;
static long g_log_size;
static pthread_mutex_t g_ext_lock;
static char g_ext[MAX_APPS][128];
static int g_ext_fps[MAX_APPS];              /* 0=不指定 -1=top -2=off -3=多档 >0=帧率 */
#define APP_TIERS_MAX 8
static int g_ext_tiers[MAX_APPS][APP_TIERS_MAX]; /* 多档（智能）时使用 */
static int g_ext_delays[MAX_APPS][APP_TIERS_MAX]; /* 该应用每段停留（ms），可缺省 */
static int g_ext_ndelays[MAX_APPS];
static char g_ext_rate[MAX_APPS][48];            /* 该应用触摸上报率写法，可缺省 */
static int g_ext_ntiers[MAX_APPS];
static unsigned char g_ext_flags[MAX_APPS];
static unsigned char g_ext_game[MAX_APPS];   /* 1=游戏条目（无刷新率） */
#define EXTF_NOGAME 0x01                /* nogame：不当作游戏 */
#define EXTF_GAME   0x02                /* game：即使带刷新率也按游戏处理 */
static int g_ext_count;

static int g_game_front;
static int g_app_fps;                   /* 前台应用单独刷新率：0=无 -1=top -2=off -3=多档 >0=帧率 */
static int g_fg_tiers[APP_TIERS_MAX];   /* 前台应用的多档（智能） */
static int g_fg_delays[APP_TIERS_MAX];  /* 前台应用每段停留（ms），空则用全局 */
static int g_fg_ndelays;
static char g_fg_rate[48];              /* 前台应用上报率写法，空则用全局 */
static int g_fg_ntiers;
static int g_app_state = -1;            /* 应用多档当前下标 */
static long long g_app_next_down;
static char g_fg_pkg[128];
static volatile int g_fg_pending;
static int g_game_active;

/* ---- 配置 ---- */
static int cfg_fps_cap;                 /* 最高刷新率，0 = 本机最高 */
static char cfg_bright_low_mode[8] = "lock";  /* lock = 低亮度锁定单档；dyn = 动态档位 */
static char cfg_anim_boost[8] = "cap";  /* cap = 本机最高刷新率；ladder = 档位最高档 */
static int cfg_bright_low = 8;   /* 亮度阈值百分比（相对系统亮度滑块 0－255），0 = 关闭 */
static int cfg_bright_low_cap = 144;    /* 低亮度时刷新率上限(Hz) */
static int g_low_cap;                   /* 当前生效的低亮度上限，0 = 未触发 */
static long long g_low_next;            /* 下次亮度检查时间 */
static int g_low_active;                /* 低亮度档位表是否已替换 */
static volatile int g_low_reapply;      /* 请求主循环立即重设档位/重放 */
static int g_low_want_cfg;             /* 进入低亮度时的配置上限值，用于判断配置是否改动 */
static int g_fg_forced;                /* 当前前台应用锁定的帧率（0 = 未锁定） */
#define TIER_AUTO (-1)                  /* 档位表里的「自适应」项 */
static long long g_auto_next;           /* 自适应档下次重新评估时间 */
static int g_orig_ntiers;               /* 配置生成的原始档位表（低亮度压缩前） */
static int g_orig_tiers[8];
static int g_orig_delays[8];
static char cfg_smart_tiers[128];            /* 智能模式档位，逗号分隔，空=top/mid/low */
static char cfg_smart_delays[128] = "1000,2000"; /* 每段停留 ms */
static volatile long long g_anim_last;   /* 最近一次动画日志时间 */
static volatile int g_anim_on;
static int g_anim_saved_fps;   /* 动画前应用单独锁定的帧率，0=无 */
static long long g_anim_restore_due;  /* 加速保持时间 */
#ifdef RRS_LEGACY_1035
#define g_no_tiers 0
#else
static int g_no_tiers = 0;   /* smart_tiers 未指定：模块不干预，交还系统动态 */
static int cfg_sys_default = 1;         /* 1 = 系统默认策略：模块不干预 */
static int g_sys_default_restored = 0;
static char g_sys_fps_list[128] = "";   /* 系统 fpsList（来自 sys_defaults） */
#endif
static char cfg_sampling[192] = "off";      /* thp_report_rate_normal（默认不改） */
static char cfg_sampling_game[192] = "off";  /* 游戏 */
static int samp_high = 480, samp_low = 240, samp_thresh = 120;
static int samp_fps[16], samp_rate[16], samp_count, samp_all, samp_auto, samp_enable;
static int gsamp_fps[16], gsamp_rate[16], gsamp_count, gsamp_all, gsamp_auto, gsamp_enable;
static char cfg_sampling_node[256];
static char cfg_sampling_cmd[128] = "8 3007 %d 1";
/* 额外采样率写入方案（适配不同触控 IC）：sampling_node2/cmd2 … 4 */
#define SAMP_PROFILES 4
static char cfg_samp_node_x[3][128];
static char cfg_samp_cmd_x[3][128];
static int g_last_samp_rate[2] = { -1, -1 };
static int g_dryrun = 0;
static int g_probe_mode;   /* --probe 只读探测，不写日志 */
static int g_ignore_touch = 0;          /* 调试：忽略触摸事件，便于验证降档 */
static int cfg_log = 0;                 /* 默认关闭；1=写日志 */

static int g_touch_contact_cap;        /* 0=无接触事件（退化为任意事件即触摸）1=有 */
static int g_state_known(int state) { return state >= 0; }
static int cfg_auto_gamelist = 1;       /* 启动时自动生成本机游戏列表 */
static char cfg_game_fps[16] = "top";   /* 游戏刷新率策略：off(默认)/top/smart/数值 */

/* 保存的原始 settings（空串表示原本是 null） */
static char g_orig_peak[32], g_orig_min[32], g_orig_user[32], g_orig_smart[8];
static int g_saved_settings = 0;
static int g_changed_settings = 0;
static int g_changed_pref = 0;

/* ===================== 基础工具 ===================== */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


static void log_msg(const char *fmt, ...)
{
    if (!cfg_log || g_probe_mode)
        return;

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    pthread_mutex_lock(&g_log_lock);
    /* 文件被删除/替换（如 WebUI 删日志）时自动重开，否则会一直写进已删除的 inode */
    if (g_log_fd >= 0) {
        struct stat fst, pst;
        if (fstat(g_log_fd, &fst) == 0 &&
            (fst.st_nlink == 0 ||
             (stat(LOG_PATH, &pst) == 0 &&
              (pst.st_ino != fst.st_ino || pst.st_dev != fst.st_dev)))) {
            close(g_log_fd);
            g_log_fd = -1;
            g_log_size = 0;
        }
    }
    if (g_log_fd < 0) {
        g_log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_log_fd >= 0) {
            struct stat st;
            g_log_size = (stat(LOG_PATH, &st) == 0) ? (long)st.st_size : 0;
        }
    }

    char line[640];
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d %s\n",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
    if (n > 0) {
        if (g_log_fd >= 0 && g_log_size + n > 262144) {   /* 轮转 */
            close(g_log_fd);
            g_log_fd = -1;
            char old[128];
            snprintf(old, sizeof old, "%s.1", LOG_PATH);
            rename(LOG_PATH, old);
            g_log_size = 0;
        }
        if (g_log_fd < 0)
            g_log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_log_fd >= 0) {
            if (write(g_log_fd, line, (size_t)n) == n) {
                g_log_size += n;
            } else {
                close(g_log_fd);
                g_log_fd = -1;
            }
        }
    }
    pthread_mutex_unlock(&g_log_lock);

    fprintf(stderr, "RRS: %s\n", msg);
}

static char *trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p))
        p++;
    char *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return p;
}

static int read_file(const char *path, char *buf, size_t sz)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

static void rotate_log(void)
{
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > 262144) {
        char old[256];
        snprintf(old, sizeof old, "%s.1", LOG_PATH);
        rename(LOG_PATH, old);
    }
}


static void load_config(void)
{
    /* 重载时先回到默认值，避免删掉的键仍保留旧值 */
    cfg_fps_cap = 0;
    cfg_bright_low = 8;
    snprintf(cfg_bright_low_mode, sizeof cfg_bright_low_mode, "lock");
    snprintf(cfg_anim_boost, sizeof cfg_anim_boost, "cap");
    cfg_bright_low_cap = 144;
#ifndef RRS_LEGACY_1035
    cfg_sys_default = 1;
#endif
    cfg_log = 0;
    cfg_auto_gamelist = 1;
    g_last_samp_rate[0] = g_last_samp_rate[1] = -1;
    snprintf(cfg_game_fps, sizeof cfg_game_fps, "top");
    cfg_smart_tiers[0] = '\0';
    snprintf(cfg_smart_delays, sizeof cfg_smart_delays, "1000,2000");
    snprintf(cfg_sampling, sizeof cfg_sampling, "off");
    snprintf(cfg_sampling_game, sizeof cfg_sampling_game, "off");
    cfg_sampling_node[0] = '\0';
    snprintf(cfg_sampling_cmd, sizeof cfg_sampling_cmd, "8 3007 %%d 1");
    memset(cfg_samp_node_x, 0, sizeof cfg_samp_node_x);
    memset(cfg_samp_cmd_x, 0, sizeof cfg_samp_cmd_x);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (f == NULL)
        return;

    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        char *k = trim(s);
        char *v = trim(eq + 1);

        if (strcmp(k, "fps_cap") == 0)
            cfg_fps_cap = atoi(v);
        else if (strcmp(k, "bright_low") == 0) {
            int b = atoi(v);
            if (b < 0) b = 0;
            if (b > 100) b = 100;   /* 系统亮度滑块的百分比 0－100 */
            cfg_bright_low = b;
        }
        else if (strcmp(k, "anim_boost") == 0) {
            if (strcmp(v, "ladder") == 0)
                snprintf(cfg_anim_boost, sizeof cfg_anim_boost, "ladder");
            else
                snprintf(cfg_anim_boost, sizeof cfg_anim_boost, "cap");
        }
        else if (strcmp(k, "bright_low_mode") == 0) {
            if (strcmp(v, "dyn") == 0 || strcmp(v, "dynamic") == 0)
                snprintf(cfg_bright_low_mode, sizeof cfg_bright_low_mode, "dyn");
            else
                snprintf(cfg_bright_low_mode, sizeof cfg_bright_low_mode, "lock");
        }
        else if (strcmp(k, "bright_low_cap") == 0) {
            int c = atoi(v);
            if (c > 0)
                cfg_bright_low_cap = c;
        }
#ifndef RRS_LEGACY_1035
        else if (strcmp(k, "sys_default") == 0)
            cfg_sys_default = atoi(v) ? 1 : 0;
#endif
        else if (strcmp(k, "log") == 0)
            cfg_log = atoi(v);
        else if (strcmp(k, "auto_gamelist") == 0)
            cfg_auto_gamelist = atoi(v) ? 1 : 0;
        else if (strcmp(k, "smart_tiers") == 0)
            snprintf(cfg_smart_tiers, sizeof cfg_smart_tiers, "%s", v);
        else if (strcmp(k, "smart_delays") == 0) {
            if (v[0])
                snprintf(cfg_smart_delays, sizeof cfg_smart_delays, "%s", v);
        }
        else if (strcmp(k, "game_fps") == 0) {
            if (v[0])
                snprintf(cfg_game_fps, sizeof cfg_game_fps, "%s", v);
        }
        else if (strcmp(k, "thp_report_rate_normal") == 0)
            snprintf(cfg_sampling, sizeof cfg_sampling, "%s", v);
        else if (strcmp(k, "thp_report_rate_game") == 0)
            snprintf(cfg_sampling_game, sizeof cfg_sampling_game, "%s", v);
        else if (strncmp(k, "sampling_node", 13) == 0 && k[13] >= '2' && k[13] <= '4') {
            int i = k[13] - '2';
            snprintf(cfg_samp_node_x[i], sizeof cfg_samp_node_x[i], "%s", v);
        }
        else if (strncmp(k, "sampling_cmd", 12) == 0 && k[12] >= '2' && k[12] <= '4') {
            int i = k[12] - '2';
            snprintf(cfg_samp_cmd_x[i], sizeof cfg_samp_cmd_x[i], "%s", v);
        }
        else if (strcmp(k, "sampling_node") == 0) {
            if (v[0])
                snprintf(cfg_sampling_node, sizeof cfg_sampling_node, "%s", v);
        }
        else if (strcmp(k, "sampling_cmd") == 0) {
            if (v[0])
                snprintf(cfg_sampling_cmd, sizeof cfg_sampling_cmd, "%s", v);
        }
    }
    fclose(f);
}

static int thp_node_writable(void)
{
    /* 节点由安装时检测并写进 config 的 sampling_node；daemon 不再自己探测 */
    return cfg_sampling_node[0] && access(cfg_sampling_node, W_OK) == 0;
}

static void parse_one_sampling(const char *src, int game)
{
    char buf[192];
    snprintf(buf, sizeof buf, "%s", src);
    char *s = trim(buf);

    int *count = game ? &gsamp_count : &samp_count;
    int *all = game ? &gsamp_all : &samp_all;
    int *auto_ = game ? &gsamp_auto : &samp_auto;
    int *enable = game ? &gsamp_enable : &samp_enable;
    int *fpsa = game ? gsamp_fps : samp_fps;
    int *ratea = game ? gsamp_rate : samp_rate;

    *count = 0;
    *all = 0;
    *auto_ = 0;
    *enable = 0;

    if (*s == '\0' || strcasecmp(s, "off") == 0 || strcmp(s, "0") == 0)
        return;
    if (strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0) {
        *enable = 1;
        *auto_ = 1;
        return;
    }
    if (strspn(s, "0123456789") == strlen(s)) {
        int v = atoi(s);
        if (v > 0) {
            *enable = 1;
            *all = v;
        }
        return;
    }

    if (!thp_node_writable()) {
        log_msg("thp_report_rate: per-fps list needs touch_thp_ic_cmd; ignored");
        return;
    }
    char *save = NULL;
    for (char *tok = strtok_r(s, ", ", &save); tok != NULL;
         tok = strtok_r(NULL, ", ", &save)) {
        char *c = strchr(tok, ':');
        if (c == NULL)
            continue;
        *c = '\0';
        int fps = atoi(tok);
        int rate = atoi(c + 1);
        if (fps > 0 && rate > 0 && *count < 16) {
            fpsa[*count] = fps;
            ratea[*count] = rate;
            (*count)++;
        }
    }
    if (*count > 0)
        *enable = 1;
}

static const char *samp_node_of(int i)
{
    if (i == 0)
        return cfg_sampling_node[0] ? cfg_sampling_node : NULL;
    if (i >= 1 && i <= 3 && cfg_samp_node_x[i - 1][0])
        return cfg_samp_node_x[i - 1];
    return NULL;
}

static const char *samp_cmd_of(int i)
{
    if (i == 0)
        return cfg_sampling_cmd[0] ? cfg_sampling_cmd : NULL;
    if (i >= 1 && i <= 3 && cfg_samp_cmd_x[i - 1][0])
        return cfg_samp_cmd_x[i - 1];
    return i == 0 ? NULL : (cfg_sampling_cmd[0] ? cfg_sampling_cmd : NULL);
}

static void parse_sampling_config(void)
{
    parse_one_sampling(cfg_sampling, 0);
    /* 游戏采样率 on=自动：游戏内交给游戏加速侧边栏开关，模块不直写 */
    if (strcmp(cfg_sampling_game, "on") == 0) {
        gsamp_enable = 0;
        gsamp_auto = 0;
    } else {
        parse_one_sampling(cfg_sampling_game, 1);
    }
}

/* ===================== 进程执行 ===================== */

static int run_cap(char *const argv[], char *out, size_t outsz)
{
    if (out != NULL)
        out[0] = '\0';

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);
    size_t total = 0;
    if (out != NULL && outsz >= 2) {
        for (;;) {
            struct pollfd pr_fd = { fds[0], POLLIN, 0 };
            if (poll(&pr_fd, 1, 3000) <= 0)
                break;   /* 子进程卡住/泄漏管道：不无限等待 */
            ssize_t n = read(fds[0], out + total, outsz - 1 - total);
            if (n <= 0)
                break;
            total += (size_t)n;
            if (total + 1 >= outsz)
                break;
        }
        out[total] = '\0';
    }
    char sink[64];
    for (;;) {
        struct pollfd pr_fd = { fds[0], POLLIN, 0 };
        if (poll(&pr_fd, 1, 1000) <= 0)
            break;
        if (read(fds[0], sink, sizeof sink) <= 0)
            break;
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int run_quiet(char *const argv[])
{
    return run_cap(argv, NULL, 0);
}

/* ===================== settings ===================== */

static int settings_get(const char *ns, const char *key, char *out, size_t sz)
{
    char *argv[] = { "settings", "get", (char *)ns, (char *)key, NULL };
    out[0] = '\0';
    if (run_cap(argv, out, sz) < 0)
        return 0;
    char *s = trim(out);
    if (strcmp(s, "null") == 0) {
        out[0] = '\0';
        return 0;
    }
    if (s != out)
        memmove(out, s, strlen(s) + 1);
    return out[0] != '\0';
}

static int settings_get_any(const char *key, char *out, size_t sz)
{
    if (settings_get("system", key, out, sz))
        return 1;
    return settings_get("secure", key, out, sz);
}

static void settings_put(const char *ns, const char *key, const char *val)
{
    if (g_dryrun)
        return;
    char *argv[] = { "settings", "put", (char *)ns, (char *)key, (char *)val, NULL };
    run_quiet(argv);
}

static void settings_delete(const char *ns, const char *key)
{
    if (g_dryrun)
        return;
    char *argv[] = { "settings", "delete", (char *)ns, (char *)key, NULL };
    run_quiet(argv);
}

#ifndef RRS_LEGACY_1035
/* ===================== 刷新率（系统设置） ===================== */

static char g_stock_min[8], g_stock_peak[8], g_stock_user[8], g_stock_miui[8];
static int g_stock_ready;

/* 首次接管前记录系统原值；重启后优先复用快照，避免把上次的固定值当成原值 */
static void stock_capture(void)
{
    if (g_stock_ready)
        return;
    g_stock_ready = 1;
    g_stock_min[0] = g_stock_peak[0] = g_stock_user[0] = g_stock_miui[0] = '\0';
    FILE *fp = fopen("/data/adb/RRS/stock", "r");
    if (fp != NULL) {
        char line[64];
        while (fgets(line, sizeof line, fp) != NULL) {
            char *nl = strpbrk(line, "\r\n");
            if (nl != NULL)
                *nl = '\0';
            char *eq = strchr(line, '=');
            if (eq == NULL)
                continue;
            *eq = '\0';
            const char *v = eq + 1;
            if (strcmp(line, "min") == 0) snprintf(g_stock_min, sizeof g_stock_min, "%s", v);
            else if (strcmp(line, "peak") == 0) snprintf(g_stock_peak, sizeof g_stock_peak, "%s", v);
            else if (strcmp(line, "user") == 0) snprintf(g_stock_user, sizeof g_stock_user, "%s", v);
            else if (strcmp(line, "miui") == 0) snprintf(g_stock_miui, sizeof g_stock_miui, "%s", v);
        }
        fclose(fp);
        return;
    }
    settings_get("system", "min_refresh_rate", g_stock_min, sizeof g_stock_min);
    settings_get("system", "peak_refresh_rate", g_stock_peak, sizeof g_stock_peak);
    settings_get("secure", "user_refresh_rate", g_stock_user, sizeof g_stock_user);
    settings_get("secure", "miui_refresh_rate", g_stock_miui, sizeof g_stock_miui);
    if (!g_dryrun) {
        fp = fopen("/data/adb/RRS/stock", "w");
        if (fp != NULL) {
            fprintf(fp, "min=%s\npeak=%s\nuser=%s\nmiui=%s\n",
                    g_stock_min, g_stock_peak, g_stock_user, g_stock_miui);
            fclose(fp);
        }
        char buf[64];
        snprintf(buf, sizeof buf, "%s,%s,%s,%s",
                 g_stock_min, g_stock_peak, g_stock_user, g_stock_miui);
        settings_put("system", "rrs_stock", buf);
    }
}

static void settings_apply_rate(int fps)
{
    char v[8];
    snprintf(v, sizeof v, "%d", fps);
    /* 只发布目标值，min/peak/user/miui 由系统侧写入 */
    settings_put("system", "rrs_refresh_target", v);
}

static void settings_restore_auto(void)
{
    /* 只发布 auto，恢复原值由系统侧负责 */
    settings_put("system", "rrs_refresh_target", "auto");
    g_cur_fps = -1;
}
#endif

static void save_settings_once(void)
{
    if (g_saved_settings)
        return;
    settings_get("system", "peak_refresh_rate", g_orig_peak, sizeof g_orig_peak);
    settings_get("system", "min_refresh_rate", g_orig_min, sizeof g_orig_min);
    settings_get("secure", "user_refresh_rate", g_orig_user, sizeof g_orig_user);
    settings_get("system", "is_smart_fps", g_orig_smart, sizeof g_orig_smart);
    g_saved_settings = 1;
}

/* 按当前档位重新钉系统设置（设置被手动改动时调用；值一致则不写，避免自触发循环） */
static void repin_settings_if_needed(void)
{
    static long long last_check;
    if (g_dryrun || g_ntiers <= 0)
        return;
    if (last_check && now_ms() - last_check < 2000)
        return;
    last_check = now_ms();
    char cur[16];
    int need = 0;
    if (!settings_get("system", "is_smart_fps", cur, sizeof cur) || strcmp(cur, "0") != 0)
        need = 1;
    if (!need) {
        static long long last_ok;
        if (now_ms() - last_ok > 5000) {   /* 一致也记一行，避免设置频繁变化时刷屏 */
            last_ok = now_ms();
            log_msg("settings consistent: smart=0");
        }
        return;
    }
    save_settings_once();
    settings_put("system", "is_smart_fps", "0");
    g_changed_settings = 1;
    log_msg("pinned smart=0 (user/peak/min 不动)");
}

/* ===================== 显示模式 ===================== */

static void add_mode(int fps, int sf, int fw, int w, int h)
{
    if (g_nmodes >= MAX_MODES)
        return;
    for (int i = 0; i < g_nmodes; i++)
        if (g_modes[i].fps == fps && g_modes[i].sf == sf)
            return;
    g_modes[g_nmodes].fps = fps;
    g_modes[g_nmodes].sf = sf;
    g_modes[g_nmodes].fw = fw;
    g_modes[g_nmodes].w = w;
    g_modes[g_nmodes].h = h;
    g_nmodes++;
}

static void sort_modes(void)
{
    for (int i = 0; i < g_nmodes; i++) {
        int best = i;
        for (int j = i + 1; j < g_nmodes; j++)
            if (g_modes[j].fps > g_modes[best].fps)
                best = j;
        if (best != i) {
            Mode t = g_modes[i];
            g_modes[i] = g_modes[best];
            g_modes[best] = t;
        }
    }
    /* 同一 fps 只保留第一个 */
    int n = 0;
    for (int i = 0; i < g_nmodes; i++) {
        int dup = 0;
        for (int j = 0; j < n; j++)
            if (g_modes[j].fps == g_modes[i].fps) { dup = 1; break; }
        if (!dup)
            g_modes[n++] = g_modes[i];
    }
    g_nmodes = n;
}

static void parse_modes_text(const char *txt)
{
    const char *p = strstr(txt, "displayId ");
    if (p != NULL)
        g_display_id = atoi(p + 10);

    const char *q = txt;
    while ((q = strstr(q, "{id=")) != NULL) {
        const char *end = strchr(q, '}');
        if (end == NULL)
            break;
        char block[512];
        size_t len = (size_t)(end - q + 1);
        if (len >= sizeof block)
            len = sizeof block - 1;
        memcpy(block, q, len);
        block[len] = '\0';

        int id = 0, sf = -1, w = 0, h = 0;
        double fps = 0;
        char *t;
        if ((t = strstr(block, "id=")) != NULL)
            id = atoi(t + 3);
        if ((t = strstr(block, "sfModeId=")) != NULL)
            sf = atoi(t + 9);
        if ((t = strstr(block, "width=")) != NULL)
            w = atoi(t + 6);
        if ((t = strstr(block, "height=")) != NULL)
            h = atoi(t + 7);
        if ((t = strstr(block, "fps=")) != NULL)
            fps = atof(t + 4);

        int f = (int)(fps + 0.5);
        if (sf < 0 && id > 0) {
            sf = id + g_sf_auto_offset;   /* Xiaomi fw-1；ColorOS 与 fw 相同 */
            if (sf < 0)
                sf = 0;
        }
        if (sf >= 0 && f > 0)
            add_mode(f, sf, id, w, h);
        q = end;
    }
}

/* 最高刷新率：面板上限与本机设置取较小值 */
static int effective_cap(void)
{
    int cap = g_support_cap > 0 ? g_support_cap : 0;
    if (cfg_fps_cap > 0 && (cap == 0 || cfg_fps_cap < cap))
        cap = cfg_fps_cap;
    return cap;
}

/* 按当前上限选 top/mid/low；配置热重载时也会重算 */
static void compute_tiers(void)
{
    if (g_nmodes == 0)
        return;

    int cap = effective_cap();
    if (cap > 0) {
        int best = -1;
        for (int i = 0; i < g_nmodes; i++)
            if (g_modes[i].fps <= cap && (best < 0 || g_modes[i].fps > best))
                best = g_modes[i].fps;
        if (best <= 0)
            best = g_modes[0].fps;
        g_top = best;
    } else {
        g_top = g_modes[0].fps;
    }

#ifdef RRS_LEGACY_1035
    g_mid = -1;
    for (int i = 0; i < g_nmodes; i++) {
        if (g_modes[i].fps <= 120 && g_modes[i].fps < g_top) {
            g_mid = g_modes[i].fps;
            break;
        }
    }
    if (g_mid < 0)
        g_mid = g_top;
#else
    /* mid 固定取 120（面板支持且低于 top 时），没有就退化为 top */
    g_mid = -1;
    for (int i = 0; i < g_nmodes; i++) {
        if (g_modes[i].fps == 120 && g_modes[i].fps <= (cap > 0 ? cap : 120) &&
            g_modes[i].fps < g_top) {
            g_mid = g_modes[i].fps;
            break;
        }
    }
    if (g_mid < 0)
        g_mid = g_top;
#endif

    g_low = -1;
    for (int i = 0; i < g_nmodes; i++) {
        if (g_modes[i].fps <= 60) {
            g_low = g_modes[i].fps;
            break;
        }
    }
    if (g_low < 0)
        g_low = g_modes[g_nmodes - 1].fps;
}

static int load_modes(void)
{
    char out[65536];

    int rc = run_cap((char *const[]){ "cmd", "display", "get-displays", NULL },
                     out, sizeof out);
    if (rc != 0 || strstr(out, "supportedModes") == NULL) {
        rc = run_cap((char *const[]){ "dumpsys", "display", NULL }, out, sizeof out);
    }

    g_nmodes = 0;
    g_display_id = 0;
    if (rc == 0)
        parse_modes_text(out);

    /* 兜底：get-supported-modes 只有 framework Mode ID，按 Xiaomi 常见
     * 关系 sf = fw - 1 猜测（仅当主解析失败时） */
    if (g_nmodes == 0) {
        char idstr[16];
        snprintf(idstr, sizeof idstr, "%d", g_display_id);
        rc = run_cap((char *const[]){ "cmd", "display", "get-supported-modes", idstr, NULL },
                     out, sizeof out);
        if (rc == 0) {
            char *line = out;
            while (*line) {
                char *nl = strchr(line, '\n');
                int id = 0;
                double hz = 0;
                if (sscanf(line, " Mode ID: %d, Resolution: %*dx%*d, Refresh Rate: %lf",
                           &id, &hz) >= 2) {
                    int f = (int)(hz + 0.5);
                    if (id > 0 && f > 0)
                        add_mode(f, id - 1, id, 0, 0);
                }
                if (nl == NULL)
                    break;
                line = nl + 1;
            }
        }
    }

    sort_modes();
    if (g_nmodes == 0)
        return -1;

    compute_tiers();

    return 0;
}

static int mode_for_fps(int fps, int nearest)
{
    int best = -1;
    for (int i = 0; i < g_nmodes; i++) {
        if (g_modes[i].fps == fps)
            return i;
        if (nearest && g_modes[i].fps <= fps && (best < 0 || g_modes[i].fps > g_modes[best].fps))
            best = i;
    }
    if (nearest && best >= 0)
        return best;
    if (nearest)
        return g_nmodes - 1;
    return -1;
}

static int read_active(int *fps, int *sf)
{
    char idstr[16];
    snprintf(idstr, sizeof idstr, "%d", g_display_id);
    char out[512];
    if (run_cap((char *const[]){ "cmd", "display", "get-active-mode", idstr, NULL },
                out, sizeof out) == 0) {
        char *p = strstr(out, "Refresh Rate:");
        if (p != NULL) {
            double hz = atof(p + 13);
            int f = (int)(hz + 0.5);
            if (fps != NULL) *fps = f;
            if (sf != NULL) {
                *sf = -1;
                for (int i = 0; i < g_nmodes; i++)
                    if (g_modes[i].fps == f) { *sf = g_modes[i].sf; break; }
            }
            return 0;
        }
    }

    /* 兼容没有 get-active-mode 的旧版 display 命令：从 get-displays 的
     * DisplayInfo 里取 "mode N" 和 renderFrameRate */
    static char big[65536];
    if (run_cap((char *const[]){ "cmd", "display", "get-displays", NULL },
                big, sizeof big) != 0)
        return -1;
    char *p = strstr(big, "DisplayInfo{");
    if (p == NULL)
        return -1;
    int fw = -1;
    char *m = strstr(p, "mode ");
    if (m != NULL)
        fw = atoi(m + 5);
    char *r = strstr(p, "renderFrameRate ");
    if (r == NULL)
        return -1;
    double hz = atof(r + 16);
    int f = (int)(hz + 0.5);
    if (f <= 0)
        return -1;
    if (fps != NULL) *fps = f;
    if (sf != NULL) {
        *sf = -1;
        for (int i = 0; i < g_nmodes; i++)
            if ((fw > 0 && g_modes[i].fw == fw) || g_modes[i].fps == f) {
                *sf = g_modes[i].sf;
                break;
            }
    }
    return 0;
}

static int set_1035(int sf)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", sf);
    char out[256];
    if (g_dryrun) {
        log_msg("DRY: service call SurfaceFlinger 1035 i32 %d", sf);
        return 0;
    }
    run_cap((char *const[]){ "service", "call", "SurfaceFlinger", "1035", "i32", buf, NULL },
            out, sizeof out);
    if (strstr(out, "Error:") != NULL) {
        log_msg("1035 i32 %d failed: %s", sf, trim(out));
        return -1;
    }
    return 0;
}

static void fallback_apply(int idx)
{
    Mode *m = &g_modes[idx];
    char val[16];
    snprintf(val, sizeof val, "%d", m->fps);
    /* 只走 DisplayManager 的 preferred mode，不改 is_smart_fps/user/peak/min：
     * 那四项由 repin_settings_if_needed() 统一维护，单应用/游戏锁帧不应触碰。 */
    char id[16], w[16], h[16];
    snprintf(id, sizeof id, "%d", g_display_id);
    snprintf(w, sizeof w, "%d", m->w);
    snprintf(h, sizeof h, "%d", m->h);
    if (m->w > 0 && m->h > 0) {
        run_quiet((char *const[]){ "cmd", "display", "set-user-preferred-display-mode",
                                   w, h, val, id, NULL });
        g_changed_pref = 1;
        log_msg("fallback: DisplayManager set %dx%d@%d", m->w, m->h, m->fps);
    }
}

/* ---- 触摸采样率（默认关闭，配置开启且回读验证） ---- */


/* 解析应用级 rate= 写法：数值 / on / 185:370,120:240（返回 1 表示命中，*out=0 表示显式 off） */
static int rate_spec_value(const char *spec, int fps, int *out)
{
    if (spec == NULL || spec[0] == '\0')
        return 0;
    if (strcasecmp(spec, "off") == 0) { *out = 0; return 1; }
    if (strcasecmp(spec, "on") == 0) { *out = (fps >= 120) ? 480 : 240; return 1; }
    if (strspn(spec, "0123456789") == strlen(spec)) { *out = atoi(spec); return 1; }
    char buf[64];
    snprintf(buf, sizeof buf, "%s", spec);
    char *sv = NULL;
    int found = 0, best = 0, best_f = -1;
    for (char *t = strtok_r(buf, ",", &sv); t != NULL; t = strtok_r(NULL, ",", &sv)) {
        char *c = strchr(t, ':');
        if (c == NULL)
            continue;
        *c = '\0';
        int f = atoi(t), r = atoi(c + 1);
        if (f <= 0 || r <= 0)
            continue;
        if (f == fps) { best = r; found = 1; break; }
        if (f >= fps && (best_f < 0 || f < best_f)) { best = r; best_f = f; found = 1; }
    }
    if (!found)
        return 0;
    *out = best;
    return 1;
}

static void set_sampling(int fps, int game)
{
    const char *node = samp_node_of(0);
    const char *tpl = samp_cmd_of(0);
    int val = 0;
    int enable = game ? gsamp_enable : samp_enable;
    int count = game ? gsamp_count : samp_count;
    int all = game ? gsamp_all : samp_all;
    int auto_ = game ? gsamp_auto : samp_auto;
    int *fpsa = game ? gsamp_fps : samp_fps;
    int *ratea = game ? gsamp_rate : samp_rate;

    /* 应用自己配了 rate= 时，即使全局表是 off 也要执行 */
    if ((!enable && g_fg_rate[0] == '\0') || node == NULL || tpl == NULL)
        return;   /* 没配节点（或安装时判定不支持）→ 只走固件改写 */

    /* 动态多档时按最高档位取映射（185→120→60 始终用 185 对应的值） */
    int map_fps = fps;
    if (g_fg_ntiers > 1) {
        int mx = g_fg_tiers[0];
        for (int i = 1; i < g_fg_ntiers; i++)
            if (g_fg_tiers[i] > mx) mx = g_fg_tiers[i];
        if (mx > 0) map_fps = mx;
    } else if (g_ntiers > 1 && g_tiers[0] > 0) {
        map_fps = g_tiers[0];
    }
    if (count > 0) {
        for (int i = 0; i < count; i++)
            if (fpsa[i] == map_fps) { val = ratea[i]; break; }
    } else if (all > 0) {
        val = all;
    } else if (auto_) {
        val = (map_fps >= samp_thresh) ? samp_high : samp_low;
    }
    if (g_fg_rate[0]) {
        int rv = 0;
        if (rate_spec_value(g_fg_rate, map_fps, &rv)) {
            if (rv == 0)
                return;             /* 该应用显式 off */
            val = rv;
        }
    }
    if (val <= 0)
        return;

    int idx = game ? 1 : 0;
    int from = g_last_samp_rate[idx];
    if (from == val)
        return;   /* 上报率没变，不重复写/不刷日志 */

    char cmd[160];
    if (strstr(tpl, "%d") != NULL)
        snprintf(cmd, sizeof cmd, tpl, val);
    else
        snprintf(cmd, sizeof cmd, "%s", tpl);

    if (g_dryrun) {
        log_msg("DRY thp_report_rate : apply %d (%s)", val, cmd);
        g_last_samp_rate[idx] = val;
        return;
    }

    int fd = open(node, O_WRONLY);
    if (fd < 0) {
        log_msg("thp_report_rate: open %s failed: %s", node, strerror(errno));
        return;
    }
    ssize_t n = write(fd, cmd, strlen(cmd));
    close(fd);
    if (n < 0) {
        log_msg("thp_report_rate: write failed: %s", strerror(errno));
        return;
    }
    g_last_samp_rate[idx] = val;
    if (from > 0)
        log_msg("thp_report_rate : apply %d [%d -> %d]", val, from, val);
    else
        log_msg("thp_report_rate : apply %d", val);
}

static int is_game_any(const char *pkg);



/* off（不干预）：先跟随应用请求的帧率，读不到再退回系统偏好 */
static void apply_system_rate(void)
{
    char out[64] = { 0 };
    int rate = 0;

    /* dumpsys display 里的 appRequest render 上界就是应用请求的帧率 */
    {
        char *big = malloc(65536);
        if (big != NULL && run_cap((char *const[]){ "dumpsys", "display", NULL },
                                   big, 65536) == 0) {
            char *p = strstr(big, "mDisplayModeSpecs=");   /* 主显示器的规格 */
            p = p ? strstr(p, "appRequest=physical:") : NULL;
            if (p != NULL) {
                char *r = strstr(p, "render:");
                if (r != NULL) {
                    char *lp = strchr(r, '(');
                    char *rp = lp ? strchr(lp, ')') : NULL;
                    if (lp != NULL && rp != NULL) {
                        double mx = 0;
                        for (char *q = lp; q < rp; q++) {
                            if (*q >= '0' && *q <= '9')
                                continue;
                            double v = strtod(q, &q);
                            if (v > mx)
                                mx = v;
                        }
                        rate = (int)(mx + 0.5);
                    }
                }
            }
            free(big);
        }
    }
    if (rate > 0) {
        int ok = 0;
        for (int i = 0; i < g_nmodes; i++)
            if (g_modes[i].fps == rate) { ok = 1; break; }
        if (!ok)
            rate = 0;
    }
    if (run_cap((char *const[]){ "settings", "get", "secure", "user_refresh_rate", NULL },
                out, sizeof out) == 0)
        rate = atoi(trim(out));
    if (rate <= 0) {
        if (run_cap((char *const[]){ "settings", "get", "system", "peak_refresh_rate", NULL },
                    out, sizeof out) == 0)
            rate = atoi(trim(out));
    }
    int sf = -1;
    for (int i = 0; i < g_nmodes; i++)
        if (g_modes[i].fps == rate) { sf = g_modes[i].sf; break; }
    if (sf < 0)
        return;
    if (g_dryrun) {
        log_msg("DRY fps : system rate %dHz", rate);
        return;
    }
    if (set_1035(sf) == 0) {
        g_cur_fps = rate;
        log_msg("fps : system rate %dHz (sfModeId %d)", rate, sf);
    }
}

static int read_int_file(const char *p)
{
    FILE *f = fopen(p, "r");
    if (f == NULL)
        return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

/* 屏幕亮度百分比：优先 /sys/class/backlight/<dev>/{brightness,max_brightness}；
 * 读不到再退回 settings screen_brightness（按 255/4095 两档换算） */
static int read_bright_pct(void)
{
    static char dir[128];
    int maxb = -1, cur = -1;
    if (dir[0] != '\0') {
        char p[192];
        snprintf(p, sizeof p, "%s/max_brightness", dir);
        maxb = read_int_file(p);
        snprintf(p, sizeof p, "%s/brightness", dir);
        cur = read_int_file(p);
    }
    if (maxb <= 0 || cur < 0) {
        dir[0] = '\0';
        DIR *d = opendir("/sys/class/backlight");
        if (d != NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.')
                    continue;
                char base[128], p[192];
                snprintf(base, sizeof base, "/sys/class/backlight/%s", e->d_name);
                snprintf(p, sizeof p, "%s/max_brightness", base);
                if (read_int_file(p) <= 0)
                    continue;
                snprintf(dir, sizeof dir, "%s", base);
                break;
            }
            closedir(d);
        }
        if (dir[0] != '\0') {
            char p[192];
            snprintf(p, sizeof p, "%s/max_brightness", dir);
            maxb = read_int_file(p);
            snprintf(p, sizeof p, "%s/brightness", dir);
            cur = read_int_file(p);
        }
    }
    if (maxb > 0 && cur >= 0 && cur <= maxb)
        return (int)((long)cur * 100 / maxb);
    char sb[16];
    if (settings_get("system", "screen_brightness", sb, sizeof sb)) {
        int v = atoi(sb);
        if (v >= 0)
            return v > 255 ? (v * 100 + 2047) / 4095 : (v * 100 + 127) / 255;
    }
    return -1;
}

/* 系统亮度滑块原始值（0－255，与设置里的 screen_brightness 同刻度） */
static int read_bright_raw(void)
{
    char sb[16];
    if (settings_get("system", "screen_brightness", sb, sizeof sb)) {
        int v = atoi(sb);
        if (v >= 0)
            return v;
    }
    int pct = read_bright_pct();
    return pct >= 0 ? pct * 255 / 100 : -1;
}

static int apply_fps(int fps, int force)
{
    if (g_low_cap > 0 && g_fg_forced == 0 && fps > g_low_cap)
        fps = g_low_cap;   /* 应用单独锁帧时不受低亮度上限影响 */
#ifdef RRS_LEGACY_1035
    int idx = mode_for_fps(fps, 1);
    if (idx < 0)
        return -1;
    int target = g_modes[idx].fps;
    int to_sf = g_modes[idx].sf;
    if (!force && target == g_cur_fps)
        return 0;
    if (g_dryrun) {
        log_msg("DRY fps : apply %dHz (sfModeId %d)", target, to_sf);
        g_cur_fps = target;
        set_sampling(target, is_game_any(g_fg_pkg) ? 1 : 0);
        return 0;
    }
    int from = g_cur_fps;
    int ok = set_1035(to_sf);
    if (ok != 0) {
        log_msg("apply sfModeId %d failed (want %dHz); trying fallback", to_sf, target);
        fallback_apply(idx);
        ok = 0;
    }
    if (ok == 0) {
        g_cur_fps = target;
        if (from > 0)
            log_msg("fps : apply %dHz [%d -> %dHz]", target, from, target);
        else
            log_msg("fps : apply %dHz", target);
        set_sampling(target, is_game_any(g_fg_pkg) ? 1 : 0);
    }
    return ok;
#else
    if (cfg_sys_default)
        return 0;
    int idx = mode_for_fps(fps, 1);
    if (idx < 0)
        return -1;
    int target = g_modes[idx].fps;
    if (!force && target == g_cur_fps)
        return 0;
    if (g_dryrun) {
        log_msg("DRY fps : apply %dHz", target);
        g_cur_fps = target;
        set_sampling(target, is_game_any(g_fg_pkg) ? 1 : 0);
        return 0;
    }
    int from = g_cur_fps;
    settings_apply_rate(target);
    g_cur_fps = target;
    if (from > 0)
        log_msg("fps : apply %dHz [%d -> %dHz]", target, from, target);
    else
        log_msg("fps : apply %dHz", target);
    set_sampling(target, is_game_any(g_fg_pkg) ? 1 : 0);
    return 0;
#endif
}



static int game_smart_mode(void)
{
    return strcmp(cfg_game_fps, "dyn") == 0 || strcmp(cfg_game_fps, "smart") == 0;
}

static void apply_game_fps(void)
{
    if (cfg_game_fps[0] == '\0' || strcmp(cfg_game_fps, "off") == 0 ||
        strcmp(cfg_game_fps, "dyn") == 0 || strcmp(cfg_game_fps, "smart") == 0)
        return;
    if (strcmp(cfg_game_fps, "top") == 0) {
        apply_fps(g_tiers[0], 1);
        return;
    }
    int hz = atoi(cfg_game_fps);
    if (hz > 0)
        apply_fps(hz, 1);
}

#ifndef RRS_LEGACY_1035
static void load_sys_defaults(void)
{
    g_sys_fps_list[0] = '\0';
    FILE *f = fopen("/data/adb/RRS/sys_defaults", "r");
    if (f == NULL)
        return;
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        char *nl = strpbrk(line, "\r\n");
        if (nl != NULL)
            *nl = '\0';
        if (strncmp(line, "fps_list=", 9) == 0)
            snprintf(g_sys_fps_list, sizeof g_sys_fps_list, "%s", line + 9);
    }
    fclose(f);
}
#endif

static void build_smart_tiers(void)
{
    g_ntiers = 0;
#ifndef RRS_LEGACY_1035
    g_no_tiers = (cfg_smart_tiers[0] == '\0');
#endif

    if (cfg_smart_tiers[0] != '\0') {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", cfg_smart_tiers);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ", ", &save); tok != NULL;
             tok = strtok_r(NULL, ", ", &save)) {
            if (strcmp(tok, "auto") == 0 || strcmp(tok, "自适应") == 0) {
                if (g_ntiers == 0 || g_tiers[g_ntiers - 1] != TIER_AUTO)
                    g_tiers[g_ntiers++] = TIER_AUTO;   /* 自适应固定放最后 */
                continue;
            }
            if (g_ntiers > 0 && g_tiers[g_ntiers - 1] == TIER_AUTO)
                continue;                              /* 自适应之后不再接受数值档 */
            int v = atoi(tok);
            if (v <= 0)
                continue;
            int cap = effective_cap();
            if (cap > 0 && v > cap)
                continue;
            int idx = mode_for_fps(v, 1);
            int f = g_modes[idx].fps;
            if (g_ntiers == 0 || f < g_tiers[g_ntiers - 1])
                g_tiers[g_ntiers++] = f;
            if (g_ntiers >= 8)
                break;
        }
    }

    if (g_ntiers == 0) {
        g_tiers[g_ntiers++] = g_top;
        if (g_mid != g_top)
            g_tiers[g_ntiers++] = g_mid;
        if (g_low != g_tiers[g_ntiers - 1])
            g_tiers[g_ntiers++] = g_low;
    }

    /* 只选「自适应」等价于「最高刷新率, 自适应」：
     * 停留时间与其他配置一致，取 smart_delays 的最后一段值 */
    if (g_ntiers == 1 && g_tiers[0] == TIER_AUTO) {
        int top = effective_cap();
        if (top <= 0)
            top = g_top;
        int idx = mode_for_fps(top, 1);
        g_tiers[0] = idx >= 0 ? g_modes[idx].fps : top;
        g_tiers[1] = TIER_AUTO;
        g_ntiers = 2;
    }

    /* 每段停留时间；不够就沿用最后一个 */
    int d[8];
    int nd = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "%s", cfg_smart_delays);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", ", &save); tok != NULL && nd < 8;
         tok = strtok_r(NULL, ", ", &save)) {
        int v = atoi(tok);
        if (v > 0)
            d[nd++] = v;
    }
    for (int i = 0; i < 8; i++) {
        if (i < nd)
            g_delays[i] = d[i];
        else if (nd > 0)
            g_delays[i] = d[nd - 1];
        else
            g_delays[i] = 3000;
    }
    /* 档位段数比配置的停留时间少时，对齐到最后一组值：
     * 例如 185,自适应 + 1000,2000 → 这一段用 2000 */
    {
        int ns = g_ntiers > 1 ? g_ntiers - 1 : 0;
        if (nd > ns && ns > 0) {
            for (int j = 0; j < ns; j++)
                g_delays[j] = d[nd - ns + j];
            for (int j = ns; j < 8; j++)
                g_delays[j] = d[nd - 1];
        }
    }
    g_orig_ntiers = g_ntiers;
    memcpy(g_orig_tiers, g_tiers, sizeof g_orig_tiers);
    memcpy(g_orig_delays, g_delays, sizeof g_orig_delays);
}

/* 低亮度模式：档位表 = cap + 原始档位里比 cap 低的档（尽量保留原阶梯） */
static void low_enter(int want)
{
    int c = want > 0 ? want : 0;
    if (c <= 0) {   /* 自动：最高档降一档；只有一档时取 60（本机支持才用） */
        if (g_orig_ntiers >= 2)
            c = g_orig_tiers[1];
        else
            c = g_orig_tiers[0] > 60 ? 60 : g_orig_tiers[0];
    }
    g_low_want_cfg = want;
    /* 上限值只按本机实际支持的模式对齐，不做任何额外降档 */
    {
        int idx = mode_for_fps(c, 1);
        if (idx >= 0)
            c = g_modes[idx].fps;
        /* 上限高于本机档位表最高档时按档位表最高档处理，避免凭空多出一档 */
        if (!g_orig_ntiers || c > g_orig_tiers[0])
            c = g_orig_ntiers ? g_orig_tiers[0] : c;
    }
    int lt[8], nd = 0;
    lt[nd++] = c;
    if (strcmp(cfg_bright_low_mode, "dyn") == 0) {
        for (int i = 0; i < g_orig_ntiers && nd < 8; i++) {
            int t = g_orig_tiers[i];
            if (t >= c || t == lt[nd - 1])
                continue;
            lt[nd++] = t;
        }
    }
    g_ntiers = nd;
    for (int i = 0; i < nd; i++)
        g_tiers[i] = lt[i];
    for (int i = 0; i < nd - 1; i++)
        g_delays[i] = i < g_orig_ntiers - 1 ? g_orig_delays[i]
                                            : g_orig_delays[g_orig_ntiers - 1];
    g_low_cap = c;
    g_low_active = 1;
}

static void low_leave(void)
{
    if (!g_low_active)
        return;
    memcpy(g_tiers, g_orig_tiers, sizeof g_tiers);
    memcpy(g_delays, g_orig_delays, sizeof g_delays);
    g_ntiers = g_orig_ntiers;
    g_low_cap = 0;
    g_low_active = 0;
}

static void tier_fmt(char *out, size_t sz, const int *tiers, int n)
{
    int off = 0;
    out[0] = '\0';
    for (int i = 0; i < n && off < (int)sz - 8; i++) {
        off += snprintf(out + off, sz - off, "%s", i ? " -> " : "");
        if (tiers[i] == TIER_AUTO)
            off += snprintf(out + off, sz - off, "auto");
        else
            off += snprintf(out + off, sz - off, "%d", tiers[i]);
    }
}

static void reload_config(void)
{
    load_config();
    compute_tiers();
#ifndef RRS_LEGACY_1035
    load_sys_defaults();
    g_sys_default_restored = 0;
#endif
    {
        char brand[64] = "";
        run_cap((char *const[]){ "getprop", "ro.product.brand", NULL }, brand, sizeof brand);
        if (brand[0] == '\0')
            run_cap((char *const[]){ "getprop", "ro.product.manufacturer", NULL }, brand, sizeof brand);
        if (strstr(brand, "oppo") || strstr(brand, "OPPO") || strstr(brand, "OnePlus") ||
            strstr(brand, "realme") || strstr(brand, "oplus"))
            g_sf_auto_offset = 0;
        else
            g_sf_auto_offset = -1;
    }
    parse_sampling_config();
    build_smart_tiers();
    if (g_low_active && cfg_bright_low > 0) {
        low_enter(cfg_bright_low_cap);
        g_low_reapply = 1;
    } else if (g_low_active) {
        low_leave();
        g_low_reapply = 1;
    }
    char tstr[80];
    tier_fmt(tstr, sizeof tstr, g_tiers, g_ntiers);
    log_msg("config reloaded: smart %s, thp_report_rate normal=%s game=%s",
            tstr, cfg_sampling, cfg_sampling_game);
}

/* ===================== device_features ===================== */

static int parse_feature_cap(const char *buf)
{
    const char *p = strstr(buf, "name=\"support_max_fps\"");
    if (p != NULL) {
        const char *q = strchr(p, '>');
        if (q != NULL) {
            int v = atoi(q + 1);
            if (v > 0)
                return v;
        }
    }
    /* 没有 support_max_fps 时退回 fpsList 最高档 */
    p = strstr(buf, "name=\"fpsList\"");
    if (p != NULL) {
        const char *e = strstr(p, "</integer-array>");
        int m = 0;
        const char *r = p;
        while (r != NULL && e != NULL && r < e) {
            r = strstr(r, "<item>");
            if (r == NULL || r > e)
                break;
            int v = atoi(r + 6);
            if (v > m)
                m = v;
            r += 6;
        }
        if (m > 0)
            return m;
    }
    return 0;
}

static int load_support_cap(void)
{
    char dev[128] = "";
    char out[256];
    if (run_cap((char *const[]){ "getprop", "ro.product.device", NULL }, out, sizeof out) == 0)
        snprintf(dev, sizeof dev, "%s", trim(out));

    static const char *dirs[] = {
        "/product/etc/device_features",
        "/system/product/etc/device_features",
        "/vendor/etc/device_features",
    };

    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        char path[512];
        if (dev[0]) {
            snprintf(path, sizeof path, "%s/%s.xml", dirs[i], dev);
            char *buf = malloc(262144);
            if (buf == NULL)
                return 0;
            if (read_file(path, buf, 262144) > 0) {
                int v = parse_feature_cap(buf);
                free(buf);
                if (v > 0)
                    return v;
            }
            free(buf);
        }

        DIR *d = opendir(dirs[i]);
        if (d == NULL)
            continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t l = strlen(de->d_name);
            if (l < 5 || strcmp(de->d_name + l - 4, ".xml") != 0)
                continue;
            snprintf(path, sizeof path, "%s/%s", dirs[i], de->d_name);
            char *buf = malloc(262144);
            if (buf == NULL)
                break;
            if (read_file(path, buf, 262144) > 0) {
                int v = parse_feature_cap(buf);
                free(buf);
                if (v > 0) {
                    closedir(d);
                    return v;
                }
            }
            free(buf);
        }
        closedir(d);
    }
    return 0;
}

/* ===================== 触摸设备发现 ===================== */

struct idev {
    char name[256];
    char event[32];
};

static int parse_input_devices(struct idev *list, int max)
{
    char buf[32768];
    if (read_file("/proc/bus/input/devices", buf, sizeof buf) < 0)
        return 0;

    int n = 0;
    char name[256] = "", ev[32] = "";
    char *line = buf;

    while (line != NULL && *line) {
        char *nl = strchr(line, '\n');
        if (nl != NULL)
            *nl = '\0';

        if (strncmp(line, "N: Name=", 8) == 0) {
            snprintf(name, sizeof name, "%s", line + 8);
            char *a = strchr(name, '"');
            char *b = strrchr(name, '"');
            if (a != NULL && b != NULL && b > a) {
                *b = '\0';
                memmove(name, a + 1, strlen(a + 1) + 1);
            }
        } else if (strncmp(line, "H: Handlers=", 12) == 0) {
            char *p = strstr(line + 12, "event");
            if (p != NULL) {
                size_t i = 0;
                while (*p && !isspace((unsigned char)*p) && i < sizeof ev - 1)
                    ev[i++] = *p++;
                ev[i] = '\0';
            }
        }

        if (*line == '\0') {
            if (name[0] && ev[0] && n < max) {
                snprintf(list[n].name, sizeof list[n].name, "%s", name);
                snprintf(list[n].event, sizeof list[n].event, "%s", ev);
                n++;
            }
            name[0] = '\0';
            ev[0] = '\0';
        }

        if (nl == NULL)
            break;
        line = nl + 1;
    }
    if (name[0] && ev[0] && n < max) {
        snprintf(list[n].name, sizeof list[n].name, "%s", name);
        snprintf(list[n].event, sizeof list[n].event, "%s", ev);
        n++;
    }
    return n;
}

static int has_bit(const unsigned long *bits, int bit)
{
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

static int is_mt_touch(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;

    unsigned long evbits[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0) {
        close(fd);
        return 0;
    }
    int ret = 0;
    if (has_bit(evbits, EV_ABS)) {
        unsigned long absbits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) >= 0) {
            if (has_bit(absbits, ABS_MT_POSITION_X) || has_bit(absbits, ABS_X))
                ret = 1;
        }
    }
    close(fd);
    return ret;
}

static int dev_score(const char *name)
{
    char low[256];
    size_t i;
    for (i = 0; name[i] && i < sizeof low - 1; i++)
        low[i] = (char)tolower((unsigned char)name[i]);
    low[i] = '\0';

    int score = 0;
    if (strstr(low, "touchscreen"))
        score += 100;
    if (strstr(low, "touch"))
        score += 30;
    static const char *keys[] = { "goodix", "focal", "novatek", "synaptics", "atmel", "nvt", "thp" };
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
        if (strstr(low, keys[k]) != NULL)
            score += 20;
    if (strstr(low, "keyboard") || strstr(low, "mouse") || strstr(low, "pen"))
        score -= 40;
    return score;
}

static int open_touch(void)
{
    if (g_touch_fd >= 0)
        return 0;

    struct idev list[MAX_IDEV];
    int n = parse_input_devices(list, MAX_IDEV);

    int best = -1, best_score = -1000;
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%s", list[i].event);
        int mt = is_mt_touch(path);
        int score = dev_score(list[i].name) + (mt ? 60 : 0);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best < 0)
        return -1;

    char path[64];
    snprintf(path, sizeof path, "/dev/input/%s", list[best].event);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        log_msg("open touch %s failed: %s", path, strerror(errno));
        return -1;
    }
    g_touch_fd = fd;

    /* 探测是否上报“接触/抬起”事件：有才按接触判断，避免 hover/噪声事件把档位一直顶在最高 */
    g_touch_contact_cap = 0;
    {
        unsigned long eb[(EV_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof eb), eb) >= 0 && has_bit(eb, EV_KEY)) {
            unsigned long kb[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb) >= 0 && has_bit(kb, BTN_TOUCH))
                g_touch_contact_cap = 1;
        }
        unsigned long ab[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof ab), ab) >= 0 &&
            has_bit(ab, ABS_MT_TRACKING_ID))
            g_touch_contact_cap = 1;
    }
    static char last_touch_log[80];
    if (strcmp(last_touch_log, path) != 0) {
        snprintf(last_touch_log, sizeof last_touch_log, "%s", path);
        log_msg("touch device: %s [%s] contact=%d", path, list[best].name,
                g_touch_contact_cap);
    }
    snprintf(g_touch_path, sizeof g_touch_path, "%s", path);
    return 0;
}

/* ===================== 游戏列表 ===================== */

static const char *const k_pkgs[] = {
    "com.tencent.tmgp.sgame", "com.tencent.tmgp.sgamece",
    "com.levelinfinite.sgameGlobal", "com.levelinfinite.sgameGlobal.midaspay",
    "com.tencent.lolm", "com.tencent.jkchess", "com.tencent.tmgp.wzsy",
    "com.tencent.tmgp.pubgmhd", "com.tencent.tmgp.cod", "com.tencent.tmgp.dfm",
    "com.tencent.mf.uam", "com.tencent.tmgp.cf", "com.tencent.ig",
    "com.tencent.igce", "com.pubg.imobile", "com.pubg.krmobile",
    "com.rekoo.pubgm", "com.vng.pubgmobile", "com.garena.game.codm",
    "com.tencent.KiHan", "com.tencent.tmgp.nz", "com.tencent.tmgp.speedmobile",
    "com.tencent.rmcn", "com.miHoYo.Yuanshen", "com.miHoYo.ys.bilibili",
    "com.miHoYo.ys.mi", "com.miHoYo.hkrpg", "com.miHoYo.hkrpg.bilibili",
    "com.miHoYo.Nap", "com.miHoYo.Nap.bilibili", "com.netease.dwrg",
    "com.netease.dwrg.nearme.gamecenter", "com.netease.dwrg5.vivo",
    "com.netease.l22", "com.netease.onmyoji", "com.netease.sky",
    "com.netease.yhtj", "com.netease.yhtj.nearme.gamecenter",
    "com.netease.race", "com.netease.party", "com.netease.mrzh",
    "com.netease.ko", "com.kurogame.mingchao",
    "com.kurogame.mingchao.bilibili", "com.kurogame.wutheringwaves.global",
    "com.kurogame.haru.hero", "com.pwrd.ht", "com.pwrd.ht.bilibili",
    "com.pwrd.yh", "com.hypergryph.arknights", "com.hypergryph.endfield",
    "com.axlebolt.standoff2", "com.axlebolt.standoff2.huawei",
    "com.sofunny.Sausage", "com.hottagames.yh.laohu",
    "com.riotgames.valorant", "com.lilith.game", "com.hermes.p6game",
    "com.mojang.minecraftpe", "com.supercell.clashofclans",
    "com.supercell.brawlstars", "com.activision.callofduty.shooter",
};

static const char *const k_keywords[] = {
    "tmgp.sgame", "levelinfinite.sgame", "tmgp.pubgmhd", "tmgp.cod",
    "tmgp.dfm", "tmgp.cf", "tmgp.speedmobile", "tmgp.nz", "tmgp.wzsy",
    "tencent.lolm", "tencent.jkchess", "tencent.KiHan", "tencent.mf.uam",
    "tencent.ig", "tencent.igce", "pubg.imobile", "pubg.krmobile",
    "rekoo.pubgm", "vng.pubgmobile", "garena.game.codm", "tencent.rmcn",
    "miHoYo.Yuanshen", "miHoYo.hkrpg", "miHoYo.Nap", "netease.dwrg",
    "netease.l22", "netease.onmyoji", "netease.sky", "netease.yhtj",
    "netease.race", "netease.party", "netease.mrzh", "netease.ko",
    "kurogame.mingchao", "kurogame.haru.hero", "kurogame.wuthering",
    "pwrd.ht", "pwrd.yh", "hypergryph.arknights", "hypergryph.endfield",
    "axlebolt.standoff2", "sofunny.Sausage", "riotgames.valorant",
    "hermes.p6game", "mojang.minecraftpe", "supercell.clashofclans",
    "supercell.brawlstars", "activision.callofduty.shooter",
};


/* ============ MIUI/HyperOS 游戏加速（Game Turbo）列表 ============
 * 读取 /data/data/com.miui.securitycenter/databases/gamebooster.db 的
 * gamebooster_table.package_name（!='none'），即"游戏加速"里已添加的应用。
 * 只实现读取该表所需的最小 SQLite B-tree 解析。
 */
static char g_gb[MAX_APPS][128];
static int g_gb_n;
static char g_gb_tmp[MAX_APPS][128];
static int g_gb_tmp_n;
static char g_gb_names[MAX_APPS][64];
static char g_gb_tmp_names[MAX_APPS][64];
static long long g_last_gb_ms;

static unsigned gb_be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned gb_be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

static unsigned long long gb_varint(const unsigned char **pp, const unsigned char *end)
{
    const unsigned char *p = *pp;
    unsigned long long v = 0;
    for (int i = 0; i < 9 && p < end; i++) {
        unsigned char b = *p++;
        if (i == 8) { v = (v << 8) | b; break; }
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    *pp = p;
    return v;
}

struct gbcol { unsigned long long st; const unsigned char *p; size_t n; };

static int gb_record(const unsigned char *rec, size_t len, struct gbcol *cols, int maxcols)
{
    const unsigned char *p = rec, *end = rec + len;
    unsigned long long hs = gb_varint(&p, end);
    if (hs < 1 || rec + hs > end)
        return -1;
    const unsigned char *he = rec + hs;
    const unsigned char *body = he;
    int n = 0;
    while (p < he && n < maxcols) {
        unsigned long long st = gb_varint(&p, he);
        size_t sz;
        if (st == 0) sz = 0;
        else if (st <= 4) sz = (size_t)st;
        else if (st == 5) sz = 6;
        else if (st == 6 || st == 7) sz = 8;
        else if (st == 8 || st == 9) sz = 0;
        else if (st >= 12) sz = (st & 1) ? (size_t)((st - 13) / 2) : (size_t)((st - 12) / 2);
        else return -1;
        if (body + sz > end)
            return -1;
        cols[n].st = st;
        cols[n].p = body;
        cols[n].n = sz;
        body += sz;
        n++;
    }
    return n;
}

static long long gb_int(const struct gbcol *c)
{
    long long v = 0;
    for (size_t i = 0; i < c->n; i++)
        v = (v << 8) | c->p[i];
    if (c->n > 0 && c->n < 8 && (c->p[0] & 0x80))
        v -= ((long long)1 << (c->n * 8));
    return v;
}

static int gb_text(const struct gbcol *c, char *out, size_t outsz)
{
    if (c->st < 13 || (c->st & 1) == 0)
        return -1;
    size_t n = c->n < outsz - 1 ? c->n : outsz - 1;
    memcpy(out, c->p, n);
    out[n] = '\0';
    return 0;
}

static void gb_tmp_add(const char *pkg, const char *name)
{
    if (pkg == NULL || pkg[0] == '\0' || strcmp(pkg, "none") == 0 || strlen(pkg) >= 127)
        return;
    for (int i = 0; i < g_gb_tmp_n; i++) {
        if (strcmp(g_gb_tmp[i], pkg) == 0) {
            if (name != NULL && name[0] != '\0' && g_gb_tmp_names[i][0] == '\0')
                snprintf(g_gb_tmp_names[i], sizeof g_gb_tmp_names[i], "%s", name);
            return;
        }
    }
    if (g_gb_tmp_n >= MAX_APPS)
        return;
    snprintf(g_gb_tmp[g_gb_tmp_n], 128, "%s", pkg);
    snprintf(g_gb_tmp_names[g_gb_tmp_n], 64, "%s", name ? name : "");
    g_gb_tmp_n++;
}

static void gb_scan_page(int fd, int page_no, int ps, int depth)
{
    if (depth > 8 || page_no <= 0)
        return;
    unsigned char *buf = malloc((size_t)ps);
    if (buf == NULL)
        return;
    if (pread(fd, buf, (size_t)ps, (off_t)(page_no - 1) * ps) != ps) {
        free(buf);
        return;
    }
    int hdr = (page_no == 1) ? 100 : 0;
    int type = buf[hdr];
    int ncell = (int)gb_be16(buf + hdr + 3);
    if (type == 0x0d) {          /* 表叶子页 */
        for (int i = 0; i < ncell; i++) {
            int off = (int)gb_be16(buf + hdr + 8 + i * 2);
            if (off <= 0 || off >= ps)
                continue;
            const unsigned char *p = buf + off, *e = buf + ps;
            unsigned long long plen = gb_varint(&p, e);
            (void)gb_varint(&p, e);   /* rowid */
            if (plen == 0 || plen > 512 || p + plen > e)
                continue;             /* 溢页记录一律跳过 */
            struct gbcol cols[24];
            int n = gb_record(p, (size_t)plen, cols, 24);
            if (n < 2)
                continue;
            char pkg[128], nm[64];
            if (gb_text(&cols[1], pkg, sizeof pkg) == 0) {
                nm[0] = '\0';
                if (n >= 3)
                    gb_text(&cols[2], nm, sizeof nm);
                gb_tmp_add(pkg, nm);
            }
        }
    } else if (type == 0x05) {   /* 表内部页 */
        int right = (int)gb_be32(buf + hdr + 8);
        for (int i = 0; i < ncell; i++) {
            int off = (int)gb_be16(buf + hdr + 12 + i * 2);
            if (off <= 0 || off >= ps)
                continue;
            int child = (int)gb_be32(buf + off);
            gb_scan_page(fd, child, ps, depth + 1);
        }
        gb_scan_page(fd, right, ps, depth + 1);
    }
    free(buf);
}

static int gb_root_page(int fd, int ps, const char *table)
{
    unsigned char *buf = malloc((size_t)ps);
    if (buf == NULL)
        return -1;
    if (pread(fd, buf, (size_t)ps, 0) != ps) {
        free(buf);
        return -1;
    }
    if (memcmp(buf, "SQLite format 3\0", 16) != 0) {
        free(buf);
        return -1;
    }
    int hdr = 100, type = buf[hdr];
    if (type != 0x0d) {
        free(buf);
        return -1;
    }
    int ncell = (int)gb_be16(buf + hdr + 3);
    int root = -1;
    for (int i = 0; i < ncell; i++) {
        int off = (int)gb_be16(buf + hdr + 8 + i * 2);
        if (off <= 0 || off >= ps)
            continue;
        const unsigned char *p = buf + off, *e = buf + ps;
        unsigned long long plen = gb_varint(&p, e);
        (void)gb_varint(&p, e);
        if (plen == 0 || plen > 1024 || p + plen > e)
            continue;
        struct gbcol cols[8];
        int n = gb_record(p, (size_t)plen, cols, 8);
        if (n < 4)
            continue;
        char name[64];
        if (gb_text(&cols[1], name, sizeof name) != 0)
            continue;
        if (strcmp(name, table) == 0) {
            root = (int)gb_int(&cols[3]);
            break;
        }
    }
    free(buf);
    return root;
}

/* 部分 HyperOS（平板等）不生成 gamebooster.db，游戏列表在 settings 里：
 * gamebooster_perf_level_cross_process_<pkg>=1 */
static void gb_scan_settings(const char *txt)
{
    static const char prefix[] = "gamebooster_perf_level_cross_process_";
    size_t plen = sizeof prefix - 1;
    const char *p = txt;
    while (p != NULL && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > plen && strncmp(p, prefix, plen) == 0) {
            char pkg[128];
            size_t k = len - plen;
            if (k >= sizeof pkg)
                k = sizeof pkg - 1;
            memcpy(pkg, p + plen, k);
            pkg[k] = '\0';
            char *eq = strchr(pkg, '=');
            if (eq != NULL)
                *eq = '\0';
            gb_tmp_add(pkg, NULL);
        }
        if (nl == NULL)
            break;
        p = nl + 1;
    }
}

static void load_gb_games(void)
{
    static const char *dbs[] = {
        "/data/data/com.miui.securitycenter/databases/gamebooster.db",
        "/data/data/com.miui.securityadd/databases/gamebooster.db",
    };
    g_gb_tmp_n = 0;
    int had_db = 0;
    for (size_t i = 0; i < sizeof dbs / sizeof dbs[0]; i++) {
        int fd = open(dbs[i], O_RDONLY);
        if (fd < 0)
            continue;
        unsigned char h[100];
        if (pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h ||
            memcmp(h, "SQLite format 3\0", 16) != 0) {
            close(fd);
            continue;
        }
        int ps = (int)gb_be16(h + 16);
        if (ps == 1)
            ps = 65536;
        if (ps < 512 || ps > 65536) {
            close(fd);
            continue;
        }
        int root = gb_root_page(fd, ps, "gamebooster_table");
        if (root > 0)
            gb_scan_page(fd, root, ps, 0);
        close(fd);
        if (g_gb_tmp_n > 0)
            had_db = 1;
    }

    /* 有真正的 Game Turbo 数据库时以它为准；没有才退回 settings 列表。
     * settings 里的 gamebooster_perf_level_cross_process_* 视频工具箱也会写，
     * 直接用会把红果短剧/网易云这类视频应用误判成游戏。 */
    char *out = had_db ? NULL : malloc(262144);
    if (out != NULL) {
        for (int ns = 0; ns < 2; ns++) {
            const char *n = ns ? "global" : "secure";
            if (run_cap((char *const[]){ "settings", "list", (char *)n, NULL },
                        out, 262144) == 0)
                gb_scan_settings(out);
        }
        free(out);
    }

    pthread_mutex_lock(&g_ext_lock);
    int changed = (g_gb_tmp_n != g_gb_n);
    if (!changed) {
        for (int i = 0; i < g_gb_n && !changed; i++) {
            int found = 0;
            for (int j = 0; j < g_gb_tmp_n; j++) {
                if (strcmp(g_gb_tmp[j], g_gb[i]) == 0) { found = 1; break; }
            }
            if (!found)
                changed = 1;
        }
    }
    memcpy(g_gb, g_gb_tmp, sizeof g_gb);
    memcpy(g_gb_names, g_gb_tmp_names, sizeof g_gb_names);
    g_gb_n = g_gb_tmp_n;
    pthread_mutex_unlock(&g_ext_lock);
    g_last_gb_ms = now_ms();
    if (changed)
        log_msg("gamebooster list: %d games (%s)", g_gb_n, had_db ? "db" : "settings");
}

static int is_game_any(const char *pkg)
{
    if (pkg == NULL || pkg[0] == '\0')
        return 0;

    pthread_mutex_lock(&g_ext_lock);
    for (int i = 0; i < g_ext_count; i++) {
        if (strcmp(g_ext[i], pkg) == 0) {
            int isgame = g_ext_game[i];
            pthread_mutex_unlock(&g_ext_lock);
            return isgame ? 1 : 0;
        }
    }
    for (int i = 0; i < g_gb_n; i++) {
        if (strcmp(g_gb[i], pkg) == 0) {
            pthread_mutex_unlock(&g_ext_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_ext_lock);

    for (size_t i = 0; i < sizeof k_pkgs / sizeof k_pkgs[0]; i++)
        if (strcmp(k_pkgs[i], pkg) == 0)
            return 1;
    for (size_t i = 0; i < sizeof k_keywords / sizeof k_keywords[0]; i++)
        if (strstr(pkg, k_keywords[i]) != NULL)
            return 1;
    return 0;
}

static void load_ext_games(void)
{
    char line[512];
    char tmp[MAX_APPS][128];
    int tmpfps[MAX_APPS];
    int tmpntiers[MAX_APPS];
    int tmpdelays[MAX_APPS][APP_TIERS_MAX];
    int tmpndelays[MAX_APPS];
    char tmprate[MAX_APPS][48];
    unsigned char tmpflags[MAX_APPS];
    unsigned char tmpgame[MAX_APPS];
    int tmptiers[MAX_APPS][APP_TIERS_MAX];
    int count = 0;
    const char *paths[] = {
        "/data/adb/RRS/AppList",          /* 手动段 + 末尾 #auto 自动段 */
        "/data/adb/RRS/GameList",        /* 旧文件名，仅兼容迁移 */
        "/sdcard/Android/Game/RRS_GameList",
        "/storage/emulated/0/Android/Game/RRS_GameList",
    };

    int have_applist = (access(paths[0], F_OK) == 0);

    /* mtime/size 没变就不再解析（前台切换很频繁，避免重复读文件） */
    static time_t last_mt;
    static long last_sz;
    struct stat st0;
    if (have_applist && stat(paths[0], &st0) == 0) {
        if (g_ext_count > 0 && st0.st_mtime == last_mt && (long)st0.st_size == last_sz)
            return;
        last_mt = st0.st_mtime;
        last_sz = (long)st0.st_size;
    }
    for (size_t pi = 0; pi < sizeof paths / sizeof paths[0]; pi++) {
        /* AppList 存在时不再读旧的 GameList / 旧位置，避免已删除的条目复活 */
        if (have_applist && pi >= 1)
            continue;
        FILE *f = fopen(paths[pi], "r");
        if (f == NULL)
            continue;
        int sect = -1;
        while (fgets(line, sizeof line, f) != NULL) {
            char *s = trim(line);
            if (*s == '#') {
                if (strncmp(s, "#auto", 5) == 0)
                    break;              /* 旧文件的自动段：忽略 */
                if (strstr(s, "应用") != NULL)
                    sect = 0;
                else if (strstr(s, "游戏") != NULL)
                    sect = 1;
                continue;
            }
            if (*s == '\0')
                continue;
            char *sp = s;
            while (*sp && !isspace((unsigned char)*sp))
                sp++;
            char *rest = (*sp != '\0') ? sp + 1 : sp;
            *sp = '\0';
            if (strchr(s, '.') == NULL || strlen(s) > 127)
                continue;

            /* 包名后的词：名称忽略；top/off/数字 = 单独刷新率；
             * 逗号列表 = 该应用智能档位；nogame = 不当作游戏 */
            int fps = 0, ntiers = 0, flags = 0;
            int tiers[APP_TIERS_MAX];
            int delays[APP_TIERS_MAX];
            int ndelays = 0;
            char rate[48];
            rate[0] = '\0';
            while (*rest) {
                while (*rest && isspace((unsigned char)*rest))
                    rest++;
                if (*rest == '\0')
                    break;
                char *tok = rest;
                while (*rest && !isspace((unsigned char)*rest))
                    rest++;
                if (*rest)
                    *rest++ = '\0';
                if (tok[0] == '#')
                    break;
                if (strncmp(tok, "delays=", 7) == 0) {
                    char lb[96];
                    snprintf(lb, sizeof lb, "%s", tok + 7);
                    char *sv2 = NULL;
                    for (char *t3 = strtok_r(lb, ",", &sv2); t3 != NULL;
                         t3 = strtok_r(NULL, ",", &sv2)) {
                        int x = atoi(t3);
                        if (x >= 0 && x <= 60000 && ndelays < APP_TIERS_MAX)
                            delays[ndelays++] = x;
                    }
                    continue;
                }
                if (strncmp(tok, "rate=", 5) == 0) {
                    snprintf(rate, sizeof rate, "%s", tok + 5);
                    continue;
                }
                if (strcasecmp(tok, "nogame") == 0) {
                    flags |= EXTF_NOGAME;
                    continue;
                }
                if (strcasecmp(tok, "game") == 0) {
                    flags |= EXTF_GAME;
                    continue;
                }
                if (strchr(tok, ',') != NULL) {
                    int tv[APP_TIERS_MAX], tn = 0;
                    char lbuf[128];
                    snprintf(lbuf, sizeof lbuf, "%s", tok);
                    char *sv = NULL;
                    int want_auto = 0;
                    for (char *t2 = strtok_r(lbuf, ",", &sv); t2 != NULL;
                         t2 = strtok_r(NULL, ",", &sv)) {
                        if (strcasecmp(t2, "auto") == 0 ||
                            strcmp(t2, "自适应") == 0) {
                            want_auto = 1;
                            continue;
                        }
                        int x = atoi(t2);
                        if (x > 0 && x <= 1000 && tn < APP_TIERS_MAX)
                            tv[tn++] = x;
                    }
                    if (tn > 0 || want_auto) {
                        for (int a = 1; a < tn; a++)
                            for (int b = a; b > 0 && tv[b - 1] < tv[b]; b--) {
                                int t3 = tv[b];
                                tv[b] = tv[b - 1];
                                tv[b - 1] = t3;
                            }
                        for (int a = 0; a < tn; a++)
                            tiers[a] = tv[a];
                        if (want_auto && tn < APP_TIERS_MAX) {
                            if (tn == 0) {
                                int top = effective_cap();
                                if (top <= 0)
                                    top = g_top;
                                int idx = mode_for_fps(top, 1);
                                tiers[tn++] = idx >= 0 ? g_modes[idx].fps : top;
                            }
                            tiers[tn++] = TIER_AUTO;
                        }
                        ntiers = tn;
                        fps = -3;
                    }
                    continue;
                }
                char *end = NULL;
                long v = strtol(tok, &end, 10);
                if (end != NULL && *end == '\0' && end != tok) {
                    if (v > 0 && v <= 1000)
                        fps = (int)v;
                } else if (strcasecmp(tok, "auto") == 0 || strcmp(tok, "自适应") == 0) {
                    if (ntiers == 0 && fps > 0) {
                        tiers[0] = fps;
                        ntiers = 1;
                    }
                    if (ntiers >= 1 && ntiers < APP_TIERS_MAX) {
                        tiers[ntiers++] = TIER_AUTO;
                        fps = -3;
                    } else {
                        int top = effective_cap();
                        if (top <= 0)
                            top = g_top;
                        int idx = mode_for_fps(top, 1);
                        tiers[0] = idx >= 0 ? g_modes[idx].fps : top;
                        tiers[1] = TIER_AUTO;
                        ntiers = 2;
                        fps = -3;
                    }
                } else if (strcasecmp(tok, "top") == 0) {
                    fps = -1;
                } else if (strcasecmp(tok, "off") == 0) {
                    fps = -2;
                }
            }

            int dup = 0;
            for (int i = 0; i < count; i++)
                if (strcmp(tmp[i], s) == 0) { dup = 1; break; }
            if (dup)
                continue;
            snprintf(tmp[count], sizeof tmp[count], "%s", s);
            tmpfps[count] = fps;
            tmpntiers[count] = ntiers;
            tmpflags[count] = (unsigned char)flags;
            if (sect == 1)
                tmpgame[count] = 1;
            else if (sect == 0)
                tmpgame[count] = 0;
            else
                tmpgame[count] = (fps == 0) ? 1 : 0;   /* 旧文件：无刷新率算游戏 */
            for (int a = 0; a < ntiers; a++)
                tmptiers[count][a] = tiers[a];
            tmpndelays[count] = ndelays;
            for (int a = 0; a < ndelays; a++)
                tmpdelays[count][a] = delays[a];
            snprintf(tmprate[count], sizeof tmprate[count], "%s", rate);
            count++;
            if (count >= MAX_APPS)
                break;
        }
        fclose(f);
    }

    pthread_mutex_lock(&g_ext_lock);
    memcpy(g_ext, tmp, (size_t)count * 128);
    memcpy(g_ext_fps, tmpfps, (size_t)count * sizeof tmpfps[0]);
    memcpy(g_ext_ntiers, tmpntiers, (size_t)count * sizeof tmpntiers[0]);
    memcpy(g_ext_flags, tmpflags, (size_t)count * sizeof tmpflags[0]);
    memcpy(g_ext_game, tmpgame, (size_t)count * sizeof tmpgame[0]);
    memcpy(g_ext_tiers, tmptiers, (size_t)count * sizeof tmptiers[0]);
    memcpy(g_ext_ndelays, tmpndelays, (size_t)count * sizeof tmpndelays[0]);
    memcpy(g_ext_delays, tmpdelays, (size_t)count * sizeof tmpdelays[0]);
    memcpy(g_ext_rate, tmprate, (size_t)count * sizeof tmprate[0]);
    g_ext_count = count;
    pthread_mutex_unlock(&g_ext_lock);
}

/* 前台应用在 GameList 里配置的单独刷新率：0=无 -1=top -2=off >0=帧率 */
static int ext_fps_for(const char *pkg)
{
    int r = 0;
    if (pkg == NULL || pkg[0] == '\0')
        return 0;
    pthread_mutex_lock(&g_ext_lock);
    for (int i = 0; i < g_ext_count; i++) {
        if (strcmp(g_ext[i], pkg) == 0) {
            r = g_ext_fps[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_ext_lock);
    return r;
}

/* 前台应用的多档（智能）配置；返回档位个数 */
static int ext_tiers_for(const char *pkg, int *out, int maxn)
{
    int n = 0;
    if (pkg == NULL || pkg[0] == '\0')
        return 0;
    pthread_mutex_lock(&g_ext_lock);
    for (int i = 0; i < g_ext_count; i++) {
        if (strcmp(g_ext[i], pkg) == 0) {
            n = g_ext_ntiers[i];
            if (n > maxn)
                n = maxn;
            for (int a = 0; a < n; a++)
                out[a] = g_ext_tiers[i][a];
            break;
        }
    }
    pthread_mutex_unlock(&g_ext_lock);
    return n;
}

/* 取该应用的每段停留与上报率（可缺省） */
static void ext_cfg_for(const char *pkg, int *delays, int *ndelays, char *rate, size_t sz)
{
    *ndelays = 0;
    rate[0] = '\0';
    if (pkg == NULL || pkg[0] == '\0')
        return;
    pthread_mutex_lock(&g_ext_lock);
    for (int i = 0; i < g_ext_count; i++) {
        if (strcmp(g_ext[i], pkg) == 0) {
            *ndelays = g_ext_ndelays[i];
            for (int a = 0; a < *ndelays && a < APP_TIERS_MAX; a++)
                delays[a] = g_ext_delays[i][a];
            snprintf(rate, sz, "%s", g_ext_rate[i]);
            break;
        }
    }
    pthread_mutex_unlock(&g_ext_lock);
}

/* 应用多档的第 idx 段停留：应用自己配了就用它，否则用全局 smart_delays */
static int app_delay(int idx)
{
    if (idx < 0)
        return 1000;
    if (g_fg_ndelays > 0 && idx < g_fg_ndelays)
        return g_fg_delays[idx];
    return (idx < APP_TIERS_MAX) ? g_delays[idx] : g_delays[APP_TIERS_MAX - 1];
}

/* ===================== GameList 自动生成 ===================== */

struct gl_cand { char pkg[128]; char name[64]; };

static int gl_has(const struct gl_cand *a, int n, const char *pkg)
{
    for (int i = 0; i < n; i++)
        if (strcmp(a[i].pkg, pkg) == 0)
            return 1;
    return 0;
}

static int pkg_installed(const char *list, const char *pkg)
{
    char pat[160];
    snprintf(pat, sizeof pat, "package:%s", pkg);
    size_t pl = strlen(pat);
    const char *p = list;
    while ((p = strstr(p, pat)) != NULL) {
        if (p == list || p[-1] == '\n') {
            char c = p[pl];
            if (c == '\n' || c == '\r' || c == '\0')
                return 1;
        }
        p += pl;
    }
    return 0;
}

/* 扫描本机已安装游戏（Game Turbo + 内置表），写进 GameList 的自动区；
 * 手动行原样保留，自动区每次启动重建（卸载/删除后不会残留）。 */
static int line_has_rate(const char *t)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", t);
    char *save = NULL;
    int idx = 0, rate = 0;
    for (char *tok = strtok_r(buf, " \t", &save); tok != NULL;
         tok = strtok_r(NULL, " \t", &save), idx++) {
        if (idx == 0)
            continue;
        if (strcasecmp(tok, "top") == 0 || strcasecmp(tok, "off") == 0)
            rate = 1;
        else {
            char *end = NULL;
            strtol(tok, &end, 10);
            if (end != NULL && *end == '\0' && end != tok)
                rate = 1;
        }
    }
    return rate;
}

/* 把自动识别的游戏追加到 AppList 的「游戏」段（不再写 #auto 注释） */
/* AppList 同包去重：保留字段最多的一条，位置保持在第一次出现处 */
static void dedupe_applist(void)
{
    const char *path = "/data/adb/RRS/AppList";
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    static char buf[262144];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (n == 0)
        return;

    char *line[4096];
    int cnt = 0;
    for (char *p = buf; *p && cnt < 4096;) {
        line[cnt++] = p;
        char *nl = strchr(p, '\n');
        if (nl == NULL)
            break;
        *nl = '\0';
        p = nl + 1;
    }
    char pkg[4096][128];
    int first[4096], nf[4096], best[4096];
    for (int i = 0; i < cnt; i++) {
        first[i] = 0;
        nf[i] = 0;
        best[i] = i;
        pkg[i][0] = '\0';
        char *t = trim(line[i]);
        if (*t == '\0' || *t == '#')
            continue;
        int c = 0;
        for (char *q = t; *q; q++)
            if (*q == ' ')
                c++;
        nf[i] = c + 1;
        snprintf(pkg[i], sizeof pkg[i], "%s", t);
        {
            char *sp = strchr(pkg[i], ' ');
            if (sp != NULL)
                *sp = '\0';
        }
    }
    for (int i = 0; i < cnt; i++) {
        if (pkg[i][0] == '\0')
            continue;
        int found = -1;
        for (int j = 0; j < i; j++) {
            if (pkg[j][0] != '\0' && strcmp(pkg[j], pkg[i]) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            if (nf[i] > nf[best[found]])
                best[found] = i;
        }
    }
    FILE *o = fopen("/data/adb/RRS/AppList.tmp", "w");
    if (o == NULL)
        return;
    for (int i = 0; i < cnt; i++) {
        if (pkg[i][0] == '\0') {
            fprintf(o, "%s\n", line[i]);
            continue;
        }
        int seen_before = 0;
        for (int j = 0; j < i; j++)
            if (pkg[j][0] != '\0' && strcmp(pkg[j], pkg[i]) == 0) {
                seen_before = 1;
                break;
            }
        if (seen_before)
            continue;
        fprintf(o, "%s\n", line[best[i]]);
    }
    fclose(o);
    rename("/data/adb/RRS/AppList.tmp", path);
    chmod(path, 0644);
}

static void gen_gamelist(void)
{
    const char *path = "/data/adb/RRS/AppList";
    if (!cfg_auto_gamelist)
        return;

    char *orig = NULL;
    size_t olen = 0;
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 262144) {
            orig = malloc((size_t)sz + 1);
            if (orig != NULL) {
                olen = fread(orig, 1, (size_t)sz, f);
                orig[olen] = '\0';
            }
        }
        fclose(f);
    }

    char *inst = malloc(1 << 20);
    if (inst == NULL) { free(orig); return; }
    if (run_cap((char *const[]){ "pm", "list", "packages", NULL }, inst, 1 << 20) != 0) {
        free(inst); free(orig); return;
    }

    struct gl_cand *cand = calloc(600, sizeof *cand);
    struct gl_cand *kept = calloc(600, sizeof *kept);
    char *apps = malloc(262144), *games = malloc(262144);
    if (cand == NULL || kept == NULL || apps == NULL || games == NULL) {
        free(inst); free(orig); free(cand); free(kept); free(apps); free(games);
        return;
    }
    int nc = 0, kept_n = 0;
    size_t ao = 0, go = 0;

    pthread_mutex_lock(&g_ext_lock);
    for (int i = 0; i < g_gb_n && nc < 600; i++) {
        if (!pkg_installed(inst, g_gb[i]) || gl_has(cand, nc, g_gb[i]))
            continue;
        snprintf(cand[nc].pkg, sizeof cand[nc].pkg, "%s", g_gb[i]);
        snprintf(cand[nc].name, sizeof cand[nc].name, "%s", g_gb_names[i]);
        nc++;
    }
    pthread_mutex_unlock(&g_ext_lock);
    for (size_t i = 0; i < sizeof k_pkgs / sizeof k_pkgs[0] && nc < 600; i++) {
        const char *pkg = k_pkgs[i];
        if (!pkg_installed(inst, pkg) || gl_has(cand, nc, pkg))
            continue;
        snprintf(cand[nc].pkg, sizeof cand[nc].pkg, "%s", pkg);
        nc++;
    }

    /* 按原分段重建：应用段原样保留；游戏段里无刷新率且属于自动识别的丢弃，稍后重建 */
    if (orig != NULL) {
        int in_game = 0;
        char *p = orig;
        while (*p) {
            char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
            char line[420];
            if (len > sizeof line - 1)
                len = sizeof line - 1;
            memcpy(line, p, len);
            line[len] = '\0';
            char *t = trim(line);
            if (*t == '#') {
                if (strstr(t, "应用") != NULL) in_game = 0;
                else if (strstr(t, "游戏") != NULL) in_game = 1;
                p += len;
                continue;
            }
            if (*t == '\0') { p += len; continue; }

            char pkg[128] = { 0 };
            snprintf(pkg, sizeof pkg, "%s", t);
            for (char *q = pkg; *q; q++)
                if (*q == ' ' || *q == '\t') { *q = '\0'; break; }
            int rate = line_has_rate(t);
            if (in_game && !rate && gl_has(cand, nc, pkg)) { p += len; continue; }

            char *dst = in_game ? games : apps;
            size_t *po = in_game ? &go : &ao;
            if (*po + len + 1 < 262144) {
                memcpy(dst + *po, p, len);
                *po += len;
                if (dst[*po - 1] != '\n')
                    dst[(*po)++] = '\n';
            }
            if (kept_n < 600)
                snprintf(kept[kept_n++].pkg, sizeof kept[0].pkg, "%s", pkg);
            p += len;
        }
    }

    size_t cap = ao + go + (size_t)nc * 300 + 256;
    char *out = malloc(cap);
    if (out == NULL) { free(inst); free(orig); free(cand); free(kept); free(apps); free(games); return; }
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o, "# 应用（单独刷新率）\n");
    if (ao > 0) { memcpy(out + o, apps, ao); o += ao; }
    o += (size_t)snprintf(out + o, cap - o, "\n# 游戏（本机识别 / 手动加入）\n");
    if (go > 0) { memcpy(out + o, games, go); o += go; }
    for (int i = 0; i < nc; i++) {
        if (gl_has(kept, kept_n, cand[i].pkg))
            continue;
        if (cand[i].name[0] != '\0') {
            char nm[64];
            snprintf(nm, sizeof nm, "%s", cand[i].name);
            for (char *q = nm; *q; q++)
                if (*q == '\n' || *q == '\r' || *q == '\t')
                    *q = ' ';
            o += (size_t)snprintf(out + o, cap - o, "%s %s\n", cand[i].pkg, nm);
        } else {
            o += (size_t)snprintf(out + o, cap - o, "%s\n", cand[i].pkg);
        }
    }

    if (orig == NULL || o != olen || memcmp(out, orig, olen) != 0) {
        char tmp[160];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *w = fopen(tmp, "w");
        if (w != NULL) {
            fwrite(out, 1, o, w);
            fclose(w);
            rename(tmp, path);
            chmod(path, 0644);
            log_msg("autogames: %d in game section", nc);
        }
    }

    free(inst); free(orig); free(cand); free(kept); free(apps); free(games); free(out);
}

/* ===================== 包名提取 ===================== */

static int extract_pkg(const char *line, char *out)
{
    out[0] = '\0';
    if (line == NULL)
        return -1;
    const char *p = line;
    while (*p) {
        if (*p != ' ' || p[1] != 'u' || !isalnum((unsigned char)p[2])) {
            p++;
            continue;
        }
        const char *q = p + 3;
        while (*q && isalnum((unsigned char)*q))
            q++;
        if (*q != ' ') {
            p++;
            continue;
        }
        while (*q == ' ')
            q++;
        size_t i = 0;
        while (*q && *q != '\n' && *q != ' ' && *q != '/' && *q != '}' && i < 255)
            out[i++] = *q++;
        out[i] = '\0';
        return out[0] ? 0 : -1;
    }
    return -1;
}

/* ===================== 前台/游戏线程 ===================== */

static int run_dumpsys(const char *a, const char *b, char *out, size_t sz)
{
    if (b != NULL)
        return run_cap((char *const[]){ "dumpsys", (char *)a, (char *)b, NULL }, out, sz);
    return run_cap((char *const[]){ "dumpsys", (char *)a, NULL }, out, sz);
}

/* ---- 内容自适应档位 ---- */
/* 从 gfxinfo framestats 估算当前前台应用的呈现帧率（视频 SurfaceView 可能测不到） */
static int fps_from_gfxinfo(void)
{
    if (g_fg_pkg[0] == '\0')
        return 0;
    char *buf = malloc(1 << 20);
    if (buf == NULL)
        return 0;
    if (run_cap((char *const[]){ "dumpsys", "gfxinfo", g_fg_pkg, "framestats", NULL },
                buf, 1 << 20) != 0) {
        free(buf);
        return 0;
    }
    int idx = 18;   /* FrameCompleted */
    char *h = strstr(buf, "Flags,FrameTimelineVsyncId");
    if (h != NULL) {
        int col = 0;
        int found = 0;
        for (char *q = h; *q && *q != '\n'; q++) {
            if (strncmp(q, "FrameCompleted", 14) == 0) {
                idx = col;
                found = 1;
                break;
            }
            if (*q == ',')
                col++;
        }
        if (!found)
            idx = 18;
    }
    long long vals[256];
    int n = 0;
    char *p = h != NULL ? strchr(h, '\n') : NULL;
    while (p != NULL && n < 256) {
        p++;
        if (!isdigit((unsigned char)*p))
            break;
        long long v = -1;
        int col = 0;
        for (char *q = p; *q && *q != '\n'; q++) {
            if (col == idx) {
                v = atoll(q);
                break;
            }
            if (*q == ',')
                col++;
        }
        if (v > 0)
            vals[n++] = v;
        p = strchr(p, '\n');
    }
    free(buf);
    if (n < 5)
        return 0;
    long long span = vals[n - 1] - vals[0];
    if (span <= 0)
        return 0;
    double fps = (double)(n - 1) * 1e9 / (double)span;
    int r = (int)(fps + 0.5);
    if (r < 1 || r > 300)
        return 0;
    return r;
}


#define AUTO_MAX_LAYERS 8
#define AUTO_HIST 2
struct auto_snap {
    long long ms;
    int n;
    int id[AUTO_MAX_LAYERS];
    long long frame[AUTO_MAX_LAYERS];
};
static struct auto_snap g_auto_hist[AUTO_HIST];
static int g_auto_hist_len;
static int g_auto_cand_hz;
static int g_auto_cand_hits;

/* 把测量帧率按区间归到内容帧率档：30/45/60/72/90/120/144/165/185 */
static int snap_content_fps(int f)
{
    if (f <= 0 || f > 240)
        return 0;
    if (f <= 32)  return 30;
    if (f <= 52)  return 45;
    if (f <= 66)  return 60;
    if (f <= 80)  return 72;
    if (f <= 100) return 90;
    if (f <= 130) return 120;
    if (f <= 152) return 144;
    if (f <= 175) return 165;
    return 185;
}

/* 内容帧率 → 刷新率：>60 取内容帧率；<60 取内容帧率的整数倍里 >=60 的最小值 */
static int adaptive_refresh(int content)
{
    int target;
    if (content <= 0)
        target = 60;
    else if (content > 60)
        target = content;
    else {
        int k = (60 + content - 1) / content;
        target = k * content;
    }
    int best = -1;
    for (int i = 0; i < g_nmodes; i++)
        if (g_modes[i].fps >= target && (best < 0 || g_modes[i].fps < best))
            best = g_modes[i].fps;
    if (best < 0)
        for (int i = 0; i < g_nmodes; i++)
            if (g_modes[i].fps > best)
                best = g_modes[i].fps;
    return best;
}

static int content_fps_detect(void)
{
    char *buf = malloc(1 << 20);
    if (buf == NULL)
        return -1;
    if (run_cap((char *const[]){ "dumpsys", "SurfaceFlinger", NULL }, buf, 1 << 20) != 0) {
        free(buf);
        return -1;
    }
    int best = 0;
    const char *tag = "requestedFrameRate: {";
    const char *q = buf;
    while ((q = strstr(q, tag)) != NULL) {
        q += strlen(tag);
        double v = atof(q);
        if (v > 0.5) {
            int fps = (int)(v + 0.5);
            if (fps < 300 && fps > best)
                best = fps;
        }
    }

    struct auto_snap cur;
    memset(&cur, 0, sizeof cur);
    cur.ms = now_ms();
    char layer[160] = "";
    int lid = 0;
    for (char *line = buf; line != NULL && *line != '\0';) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len > 0 && len < 190) {
            char tmp[200];
            memcpy(tmp, line, len);
            tmp[len] = '\0';
            char *lp = strstr(tmp, "Layer [");
            if (lp != NULL) {
                lid = atoi(lp + 7);
                char *nm = strstr(tmp, "] ");
                if (nm != NULL)
                    snprintf(layer, sizeof layer, "%s", nm + 2);
            } else if (strstr(layer, "SurfaceView") != NULL &&
                       strstr(layer, g_fg_pkg) != NULL &&
                       cur.n < AUTO_MAX_LAYERS) {
                char *fp = strstr(tmp, "frame=");
                if (fp != NULL) {
                    cur.id[cur.n] = lid;
                    cur.frame[cur.n] = atoll(fp + 6);
                    cur.n++;
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(buf);

    /* 用约 4 秒窗口（两次采样间隔）算各层帧率，取最大者 */
    int measured = 0;
    int window_valid = 0;
    if (g_auto_hist_len > 0 && cur.n > 0) {
        struct auto_snap *old = (g_auto_hist_len >= 2) ? &g_auto_hist[1] : &g_auto_hist[0];
        long long dt = cur.ms - old->ms;
        if (dt >= 800) {
            window_valid = 1;
            int low_fps = -1;
            int high_fps = -1;
            for (int i = 0; i < cur.n; i++)
                for (int j = 0; j < old->n; j++)
                    if (cur.id[i] == old->id[j]) {
                        long long df = cur.frame[i] - old->frame[j];
                        if (df > 0) {
                            int r = (int)((double)df * 1000.0 / (double)dt + 0.5);
                            if (r <= 60) {
                                if (r > low_fps)
                                    low_fps = r;   /* 有低帧层时优先用它（主内容） */
                            } else if (high_fps < 0 || r < high_fps) {
                                high_fps = r;      /* 没有低帧层时才用高帧层，取最低的那个 */
                            }
                        }
                        break;
                    }
            if (low_fps >= 0)
                measured = snap_content_fps(low_fps);
            else if (high_fps >= 0)
                measured = snap_content_fps(high_fps);   /* 高帧内容同样参与统计 */
            else
                measured = 0;                      /* 画面静止/暂停 → 0fps */
        }
    }

    g_auto_hist[1] = g_auto_hist[0];
    g_auto_hist[0] = cur;
    if (g_auto_hist_len < 2)
        g_auto_hist_len++;

    if (measured > 0)
        return measured;
    if (window_valid)
        return 0;    /* 窗口有效但没有任何新帧：内容静止/暂停 → 0fps */
    if (best > 0)
        return best;
    if (cur.n == 0)
        return 0;
    return -1;
}

#define AUTO_AVG_N 8
static int g_auto_avg[AUTO_AVG_N];
static int g_auto_avg_n;
static int g_auto_avg_pos;
static long long g_auto_sample_at;   /* 下一次采帧时间（1 秒间隔） */

static void auto_avg_reset(void)
{
    g_auto_avg_n = 0;
    g_auto_avg_pos = 0;
    g_auto_sample_at = 0;
}

static void auto_avg_push(int f)
{
    if (f < 0)
        return;
    g_auto_avg[g_auto_avg_pos] = f;
    g_auto_avg_pos = (g_auto_avg_pos + 1) % AUTO_AVG_N;
    if (g_auto_avg_n < AUTO_AVG_N)
        g_auto_avg_n++;
}

/* 众数：取出现次数最多的内容帧率；并列时取较大的那个 */
static int auto_avg_value(void)
{
    if (g_auto_avg_n <= 0)
        return -1;
    int take = g_auto_avg_n < 4 ? g_auto_avg_n : 4;   /* 最近 4 个样本的众数 */
    int best_v = -1, best_c = 0;
    for (int i = 0; i < take; i++) {
        int idx = (g_auto_avg_pos - 1 - i + AUTO_AVG_N) % AUTO_AVG_N;
        int v = snap_content_fps(g_auto_avg[idx]);
        if (v < 0)     /* 0fps（静止/暂停）也算有效值：映射到 60Hz */
            continue;
        int c = 0;
        for (int j = 0; j < take; j++) {
            int idx2 = (g_auto_avg_pos - 1 - j + AUTO_AVG_N) % AUTO_AVG_N;
            if (snap_content_fps(g_auto_avg[idx2]) == v)
                c++;
        }
        if (c > best_c || (c == best_c && v > best_v)) {
            best_c = c;
            best_v = v;
        }
    }
    return best_v;
}

static int apply_auto_tier(int force)
{
    int avg = auto_avg_value();
    if (avg < 0 || g_auto_avg_n <= 0)
        return 0;   /* 前档没有统计到低帧样本，保持现状 */
    int hz = adaptive_refresh(snap_content_fps(avg));
    if (hz <= 0)
        return -1;
    if (hz == g_cur_fps)
        return 0;
    log_msg("auto tier: mode %dfps -> %dHz", avg, hz);   /* 缓存值：稳定低帧对应的刷新率 */
    return apply_fps(hz, force);
}

static int tier_apply(int idx, int force)
{
    if (idx < 0 || idx >= g_ntiers)
        return -1;
    if (g_tiers[idx] == TIER_AUTO)
        return apply_auto_tier(force);
    return apply_fps(g_tiers[idx], force);
}

static int top_numeric_tier(void)
{
    for (int i = 0; i < g_ntiers; i++)
        if (g_tiers[i] != TIER_AUTO)
            return g_tiers[i];
    int mx = 0;
    for (int i = 0; i < g_nmodes; i++)
        if (g_modes[i].fps > mx)
            mx = g_modes[i].fps;
    return mx;
}

static void fmt_fps_mode(int app_fps, const int *tiers, int n, char *out, size_t sz)
{
    if (app_fps == 0) { snprintf(out, sz, "global"); return; }
    if (app_fps == -1) { snprintf(out, sz, "top"); return; }
    if (app_fps == -2) { snprintf(out, sz, "off"); return; }
    if (app_fps == -3) {
        if (tiers != NULL)
            tier_fmt(out, sz, tiers, n);
        if (out[0] == '\0') snprintf(out, sz, "off");
        return;
    }
    snprintf(out, sz, "%d", app_fps);
}

/* 游戏档位：top / 数值 / off / 动态 "185 -> 60"（不带 game 字样） */
static void fmt_game_mode(char *out, size_t sz)
{
    if (game_smart_mode()) {
        tier_fmt(out, sz, g_tiers, g_ntiers);
        if (out[0] == '\0') snprintf(out, sz, "top");
    } else if (strcmp(cfg_game_fps, "top") == 0) {
        snprintf(out, sz, "top");
    } else if (strcmp(cfg_game_fps, "off") == 0) {
        snprintf(out, sz, "off");
    } else {
        snprintf(out, sz, "%s", cfg_game_fps);
    }
}

static void fg_set_game(int game, int app_fps, const int *tiers, int ntiers)
{
    static char last_pkg[128];
    int n = ntiers < 0 ? 0 : (ntiers > APP_TIERS_MAX ? APP_TIERS_MAX : ntiers);
    int changed = (game != g_game_front) || (app_fps != g_app_fps) || (n != g_fg_ntiers);
    if (!changed) {
        for (int i = 0; i < n; i++) {
            if (tiers[i] != g_fg_tiers[i]) {
                changed = 1;
                break;
            }
        }
    }
    if (strcmp(last_pkg, g_fg_pkg) != 0) {
        snprintf(last_pkg, sizeof last_pkg, "%s", g_fg_pkg);
        g_app_state = -1;
        g_app_next_down = 0;
        changed = 1;
    }
    g_fg_pending = 1;
    g_game_front = game;
    g_app_fps = app_fps;
    g_fg_ntiers = n;
    for (int i = 0; i < n; i++)
        g_fg_tiers[i] = tiers[i];
    if (changed) {
        {
            if (app_fps != 0) {
                /* 只有「单个应用/游戏」单独指定了刷新率才打 fps_mode */
                char fm[96];
                fmt_fps_mode(app_fps, g_fg_tiers, g_fg_ntiers, fm, sizeof fm);
                log_msg("fg: %s app_type=%s fps_mode=%s", g_fg_pkg, game ? "game" : "normal", fm);
            } else {
                log_msg("fg: %s app_type=%s", g_fg_pkg, game ? "game" : "normal");
            }
        }
    }
    if (changed && g_app_fps == 0) {
        /* 固定模式：不再改刷新率，只按应用/游戏切换采样率 */
        int rfps = g_cur_fps > 0 ? g_cur_fps : 0;
        set_sampling(rfps, is_game_any(g_fg_pkg) ? 1 : 0);
    }
    if (changed && g_wake_wr >= 0) {
        char c = 1;
        if (write(g_wake_wr, &c, 1) < 0) {
            /* 非阻塞管道，满则忽略 */
        }
    }
}

/* 抓一次 dumpsys window/activity，判断前台是否游戏 */
/* 从 logcat 事件块解析前台包名（wm_set_resumed_activity: [0,pkg/.Act,...]） */
static int pkg_from_events(const char *buf, char *out, size_t sz)
{
    const char *tag = "wm_set_resumed_activity:";
    const char *p = buf;
    int got = 0;
    out[0] = '\0';
    while ((p = strstr(p, tag)) != NULL) {
        p += strlen(tag);
        const char *lb = strchr(p, '[');
        if (lb == NULL)
            break;
        const char *c = lb + 1;
        while (*c == ' ')
            c++;
        const char *comma = strchr(c, ',');      /* 跳过事件里的第一个序号字段 */
        if (comma != NULL) {
            c = comma + 1;
            while (*c == ' ')
                c++;
        }
        char tmp[128];
        size_t i = 0;
        while (*c && *c != ',' && *c != '/' && *c != '\n' && *c != ']' && i < sizeof tmp - 1)
            tmp[i++] = *c++;
        tmp[i] = '\0';
        p = c;
        if (i > 0 && strchr(tmp, '.') != NULL) {
            snprintf(out, sz, "%s", tmp);
            got = 1;
        }
    }
    return got;
}

/* 用已知包名落地前台状态（游戏判定 + 应用配置） */
static void fg_apply_pkg(const char *pkg)
{
    int game = 0;
    if (pkg[0] == '\0') {
        game = 0;
    } else if (is_game_any(pkg)) {
        game = 1;
    } else {
        game = 0;
    }
    snprintf(g_fg_pkg, sizeof g_fg_pkg, "%s", pkg);
    {
        int fg_tiers[APP_TIERS_MAX];
        int fg_ntiers = ext_tiers_for(pkg, fg_tiers, APP_TIERS_MAX);
        ext_cfg_for(pkg, g_fg_delays, &g_fg_ndelays, g_fg_rate, sizeof g_fg_rate);
        fg_set_game(game, ext_fps_for(pkg), fg_tiers, fg_ntiers);
    }
}

static void fg_poll_once(void)
{
    static char win[65536];
    static char pkg[512];
    static char focus_line[512];

    load_ext_games();

    memset(win, 0, sizeof win);
    run_dumpsys("window", NULL, win, sizeof win);

    pkg[0] = '\0';

    char *focus = strstr(win, "mCurrentFocus=");
    if (focus != NULL) {
        char *nl = strchr(focus, '\n');
        size_t len = nl ? (size_t)(nl - focus) : strlen(focus);
        if (len > sizeof focus_line - 1)
            len = sizeof focus_line - 1;
        memcpy(focus_line, focus, len);
        focus_line[len] = '\0';
        if (extract_pkg(focus_line, pkg) != 0)
            pkg[0] = '\0';
        if (strcmp(pkg, "null") == 0 || strncmp(pkg, "Window", 6) == 0)
            pkg[0] = '\0';
    }

    /* 部分 ROM 只给 mCurrentFocusedWindow = <pkg>（没有 Window{... u0}） */
    if (pkg[0] == '\0') {
        char *fw = strstr(win, "mCurrentFocusedWindow");
        if (fw != NULL) {
            char *eq = strchr(fw, '=');
            if (eq != NULL) {
                char *q = eq + 1;
                while (*q == ' ')
                    q++;
                size_t i = 0;
                while (*q && *q != '\n' && *q != '\r' && *q != ' ' && *q != '/' &&
                       i < sizeof pkg - 1)
                    pkg[i++] = *q++;
                pkg[i] = '\0';
            }
        }
    }

    /* 焦点落在系统表面（如 NotificationShade，不含包点名）时保持上一次的前台 */
    if (pkg[0] != '\0' && strchr(pkg, '.') == NULL)
        return;

    fg_apply_pkg(pkg);
}

/* 没有 logcat 时的兜底：2 秒轮询 */
static void fg_poll_loop(void)
{
    for (;;) {
        if (g_stop)
            return;
        fg_poll_once();
        for (int i = 0; i < 20; i++) {
            if (g_stop)
                return;
            poll(NULL, 0, 100);
        }
    }
}

/* 监听动画相关日志（转场/通知栏/应用启停），用于动画加速 */
static void anim_logcat_loop(void)
{
    static const char *markers[] = {
        "Transition anim start",
        "Set transition ",
        "startAnimation",
        "NotificationShade",
        "ShadeTransition",
        "miui.app.Transition",
    };
    for (;;) {
        if (g_stop)
            return;
        int sfpid = 0;
        {
            char pb[64];
            pb[0] = '\0';
            if (run_cap((char *const[]){ "pidof", "surfaceflinger", NULL },
                        pb, sizeof pb) == 0)
                sfpid = atoi(pb);
        }
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) {
            poll(NULL, 0, 500);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            /* 只订阅 SurfaceFlinger 的日志并在 logd 侧按 pid 过滤：
             * 转场动画开始由 SF 打点，覆盖面够且开销远低于整 buffer 过滤 */
            if (sfpid > 0) {
                char pbuf[16];
                snprintf(pbuf, sizeof pbuf, "%d", sfpid);
                execl("/system/bin/logcat", "logcat", "-b", "main", "-s",
                      "SurfaceFlinger", "-v", "epoch", "--pid", pbuf, (char *)NULL);
            } else {
                execl("/system/bin/logcat", "logcat", "-b", "main", "-s",
                      "SurfaceFlinger", "-v", "epoch", (char *)NULL);
            }
            _exit(127);
        }
        if (pid < 0) {
            close(fds[0]);
            close(fds[1]);
            poll(NULL, 0, 500);
            continue;
        }
        close(fds[1]);
        int fd = fds[0];
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        for (;;) {
            if (g_stop)
                break;
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int r = poll(&pfd, 1, 500);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            char buf[2048];
            ssize_t n = read(fd, buf, sizeof buf - 1);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EINTR))
                    continue;
                break;
            }
            buf[n] = '\0';
            int hit = 0;
            for (size_t i = 0; i < sizeof markers / sizeof markers[0]; i++)
                if (strstr(buf, markers[i]) != NULL) {
                    hit = 1;
                    break;
                }
            if (hit) {
                g_anim_last = now_ms();
                if (g_wake_wr >= 0) {
                    char c = 1;
                    if (write(g_wake_wr, &c, 1) < 0) {
                    }
                }
            }
        }
        if (pid > 0)
            kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        poll(NULL, 0, 500);
    }
}

/* 事件驱动：常驻 logcat -b events，只在窗口 resume 事件到来时抓一次 dumpsys；
 * 无事件时阻塞等待（不做定时轮询）；logcat 退出自动重启并立刻判定一次 */
static void fg_logcat_loop(void)
{
    for (;;) {
        if (g_stop)
            return;
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) {
            poll(NULL, 0, 500);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
            /* 父进程死掉（含 -9）时一并结束，避免孤儿 logcat 越积越多 */
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            execl("/system/bin/logcat", "logcat", "-b", "events", "-s",
                  "wm_on_top_resumed_gained_called", "wm_set_resumed_activity",
                  "wm_on_resume_called", "-v", "epoch", (char *)NULL);
            _exit(127);
        }
        if (pid < 0) {
            close(fds[0]);
            close(fds[1]);
            poll(NULL, 0, 500);
            continue;
        }
        close(fds[1]);
        int fd = fds[0];
        g_logcat_pid = pid;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

        fg_poll_once();

        for (;;) {
            if (g_stop)
                break;
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int r = poll(&pfd, 1, -1);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof buf - 1);
            if (n <= 0)
                break;            /* logcat 退出，重启 */
            buf[n] = '\0';
            /* 过滤器里的任一 resume 事件都触发一次前台判定 */
            {
                char epkg[128];
                char chunk[4096];
                snprintf(chunk, sizeof chunk, "%s", buf);
                ssize_t m;
                while ((m = read(fd, chunk, sizeof chunk - 1)) > 0)
                    chunk[m] = '\0';
                if (pkg_from_events(chunk, epkg, sizeof epkg)) {
                    fg_apply_pkg(epkg);      /* 事件里就有包名：不用再跑 dumpsys */
                } else if (pkg_from_events(buf, epkg, sizeof epkg)) {
                    fg_apply_pkg(epkg);
                } else {
                    poll(NULL, 0, 400);      /* 拿不到包名：等动画稳定后用 dumpsys 兜底 */
                    fg_poll_once();
                }
            }
        }
        close(fd);
        if (g_stop && pid > 0)
            kill(pid, SIGTERM);
        int st;
        if (pid > 0)
            waitpid(pid, &st, 0);
        if (g_stop)
            return;
        poll(NULL, 0, 1000);
    }
}

static void *anim_thread(void *arg)
{
    (void)arg;
    anim_logcat_loop();
    return NULL;
}

static void *fg_thread(void *arg)
{
    (void)arg;
    if (g_stop)
        return NULL;
    if (access("/system/bin/logcat", X_OK) == 0)
        fg_logcat_loop();
    else
        fg_poll_loop();
    return NULL;
}

/* ===================== 收尾 ===================== */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void restore_saved_settings(void)
{
    if (!g_changed_settings || g_dryrun)
        return;
    /* user/peak/min 不再由 daemon 改写，退出时也不恢复，避免覆盖用户手动修改 */
    if (g_orig_smart[0])
        settings_put("system", "is_smart_fps", g_orig_smart);
    else
        settings_delete("system", "is_smart_fps");
    g_changed_settings = 0;
    log_msg("restored settings");
}

static void restore_all(void)
{
    restore_saved_settings();
    if (g_changed_pref && !g_dryrun) {
        char id[16];
        snprintf(id, sizeof id, "%d", g_display_id);
        run_quiet((char *const[]){ "cmd", "display", "reset-user-preferred-display-mode", id, NULL });
    }
    if (!g_dryrun) {
#ifdef RRS_LEGACY_1035
        if (g_orig_sf >= 0) {
            log_msg("restore refresh to sfModeId %d", g_orig_sf);
            set_1035(g_orig_sf);
        }
#else
        log_msg("restore refresh settings to stock");
        settings_restore_auto();
#endif
    }
}

static int get_user(void)
{
    char out[64];
    if (run_cap((char *const[]){ "am", "get-current-user", NULL }, out, sizeof out) == 0) {
        int u = atoi(trim(out));
        if (u >= 0)
            return u;
    }
    return 0;
}


static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

static void probe(void)
{
    printf("user=%d display_id=%d support_cap=%d\n",
           g_user, g_display_id, g_support_cap);
    printf("modes: %d\n", g_nmodes);
    for (int i = 0; i < g_nmodes; i++)
        printf("  sf=%-2d fw=%-2d fps=%-3d %dx%d\n",
               g_modes[i].sf, g_modes[i].fw, g_modes[i].fps,
               g_modes[i].w, g_modes[i].h);
    printf("tiers: top=%d mid=%d low=%d\n", g_top, g_mid, g_low);
    printf("dyn tiers:");
    for (int i = 0; i < g_ntiers; i++)
        printf(" %d%s", g_tiers[i], (i < g_ntiers - 1) ? " ->" : "");
    printf("\n");
    for (int i = 0; i < g_ntiers - 1; i++)
        printf("  delay[%d -> %d] = %d ms\n", g_tiers[i], g_tiers[i+1], g_delays[i]);

    int af = 0, asf = -1;
    read_active(&af, &asf);
    printf("active: fps=%d sf=%d\n", af, asf);

    char v[64];
    if (settings_get_any("is_smart_fps", v, sizeof v))
        printf("is_smart_fps=%s\n", v);
    else
        printf("is_smart_fps=<unset>\n");
    if (settings_get("secure", "user_refresh_rate", v, sizeof v) ||
        settings_get("system", "user_refresh_rate", v, sizeof v))
        printf("user_refresh_rate=%s\n", v);
    else
        printf("user_refresh_rate=<unset>\n");

    printf("touch=%s fd=%d\n", g_touch_path[0] ? g_touch_path : "<none>", g_touch_fd);
    const char *node = samp_node_of(0);
    printf("thp_report_rate(daily)=%s (enable=%d) node=%s\n",
           cfg_sampling, samp_enable, node ? node : "<none>");
    if (samp_count > 0) {
        printf("  daily mapping:");
        for (int i = 0; i < samp_count; i++) printf(" %d->%d", samp_fps[i], samp_rate[i]);
        printf("\n");
    } else if (samp_all > 0) {
        printf("  daily mapping: all -> %d\n", samp_all);
    } else if (samp_auto) {
        printf("  daily mapping: auto (>=%d -> %d, else %d)\n", samp_thresh, samp_high, samp_low);
    }
    printf("thp_report_rate(game)=%s (enable=%d)\n", cfg_sampling_game, gsamp_enable);
    if (gsamp_count > 0) {
        printf("  game mapping:");
        for (int i = 0; i < gsamp_count; i++) printf(" %d->%d", gsamp_fps[i], gsamp_rate[i]);
        printf("\n");
    } else if (gsamp_all > 0) {
        printf("  game mapping: all -> %d\n", gsamp_all);
    } else if (gsamp_auto) {
        printf("  game mapping: auto (>=%d -> %d, else %d)\n", samp_thresh, samp_high, samp_low);
    }
    printf("app_list=/data/adb/RRS/AppList\n");
    printf("gamebooster(%d):", g_gb_n);
    for (int i = 0; i < g_gb_n; i++) printf(" %s", g_gb[i]);
    printf("\n");
    printf("dryrun=%d log=%d\n", g_dryrun, cfg_log);
}

int main(int argc, char **argv)
{
    int do_probe = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) {
            do_probe = 1;
            g_probe_mode = 1;
        }
        else if (strcmp(argv[i], "--dry-run") == 0)
            g_dryrun = 1;
        else if (strcmp(argv[i], "--ignore-touch") == 0)
            g_ignore_touch = 1;
    }

    mkdir("/data/adb/RRS", 0755);
    if (!do_probe && !g_dryrun) {
        /* 接管：结束旧实例，让新进程生效（更新/重启都是“新的顶掉旧的”） */
        char pids[512] = { 0 };
        if (run_cap((char *const[]){ "pidof", "RRS_Daemon", NULL }, pids, sizeof pids) == 0) {
            int self = getpid();
            int killed = 0;
            char *save = NULL;
            for (char *tok = strtok_r(pids, " \n", &save); tok != NULL;
                 tok = strtok_r(NULL, " \n", &save)) {
                int pid = atoi(tok);
                if (pid > 0 && pid != self) {
                    kill(pid, SIGTERM);
                    killed++;
                }
            }
            if (killed > 0) {
                sleep(1);
                memset(pids, 0, sizeof pids);
                if (run_cap((char *const[]){ "pidof", "RRS_Daemon", NULL }, pids, sizeof pids) == 0) {
                    for (char *tok = strtok_r(pids, " \n", &save); tok != NULL;
                         tok = strtok_r(NULL, " \n", &save)) {
                        int pid = atoi(tok);
                        if (pid > 0 && pid != self)
                            kill(pid, SIGKILL);
                    }
                }
            }
        }
        unlink("/data/adb/RRS/daemon.lock");
    }

    rotate_log();
    pthread_mutex_init(&g_log_lock, NULL);
    pthread_mutex_init(&g_ext_lock, NULL);

    load_config();
#ifndef RRS_LEGACY_1035
    load_sys_defaults();
#endif
    /* 旧文件名迁移：GameList -> AppList（保留旧文件，不删） */
    if (access("/data/adb/RRS/AppList", F_OK) != 0 &&
        access("/data/adb/RRS/GameList", F_OK) == 0) {
        FILE *src = fopen("/data/adb/RRS/GameList", "r");
        FILE *dst = fopen("/data/adb/RRS/AppList", "w");
        if (src != NULL && dst != NULL) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, src)) > 0)
                fwrite(buf, 1, n, dst);
        }
        if (src != NULL) fclose(src);
        if (dst != NULL) fclose(dst);
        chmod("/data/adb/RRS/AppList", 0644);
    }
    unlink("/data/adb/RRS/AutoGames");   /* 旧的独立自动列表，已并入 AppList 的 #auto 段 */
    parse_sampling_config();
    g_user = get_user();
    g_support_cap = load_support_cap();

    /* 开机早期 display 服务可能还没就绪：重试最多 60 秒再放弃 */
    int lmr = load_modes();
    for (int i = 0; lmr != 0 && i < 60 && !g_stop; i++) {
        sleep(1);
        lmr = load_modes();
    }
    if (lmr != 0) {
        log_msg("no display modes found; abort");
        return 2;
    }

    build_smart_tiers();
    load_gb_games();
    gen_gamelist();
    dedupe_applist();

    int af = 0;
    read_active(&af, &g_orig_sf);

    if (!g_ignore_touch)
        open_touch();

    if (do_probe) {
        probe();
        if (g_touch_fd >= 0)
            close(g_touch_fd);
        return 0;
    }

    log_msg("start: user=%d top=%d mid=%d low=%d support_cap=%d touch=%s",
            g_user, g_top, g_mid, g_low, g_support_cap,
            g_touch_path[0] ? g_touch_path : "none");

    /* 只监听模块目录与 Game Turbo 数据库；不再监听 settings（亮度/音量等无关事件太多） */
    g_inotify_fd = inotify_init1(IN_NONBLOCK);
    if (g_inotify_fd >= 0) {
        inotify_add_watch(g_inotify_fd, "/data/adb/RRS",
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        inotify_add_watch(g_inotify_fd, "/data/data/com.miui.securitycenter/databases",
                          IN_CLOSE_WRITE | IN_MOVED_TO);
        inotify_add_watch(g_inotify_fd, "/data/data/com.miui.securityadd/databases",
                          IN_CLOSE_WRITE | IN_MOVED_TO);
    }

    setup_signals();

    int wake_pipe[2] = { -1, -1 };
    if (pipe(wake_pipe) == 0) {
        fcntl(wake_pipe[0], F_SETFL, fcntl(wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
        fcntl(wake_pipe[1], F_SETFL, fcntl(wake_pipe[1], F_GETFL, 0) | O_NONBLOCK);
        g_wake_wr = wake_pipe[1];
    }

    pthread_t th;
    pthread_create(&th, NULL, fg_thread, NULL);
    {
        pthread_t ath;
        pthread_create(&ath, NULL, anim_thread, NULL);
        pthread_detach(ath);
    }

    int smart = -1, state = -1, dirty = 1, hold = 0;
    long long cfg_due = 0;   /* config 重载防抖截止时间 */
    int cur_forced = 0;      /* 当前前台应用的单独刷新率（0=无） */
    int last_custom = -1;
    time_t last_game = 0;
    long long next_down = 0;

    while (!g_stop) {
        time_t now = time(NULL);

#ifndef RRS_LEGACY_1035
        if (cfg_sys_default) {
            if (!g_sys_default_restored) {
                settings_restore_auto();
#ifdef RRS_LEGACY_1035
                {
                    char pref[16] = "";
                    int hz = 0;
                    if (settings_get("secure", "user_refresh_rate", pref, sizeof pref))
                        hz = atoi(pref);
                    if (hz <= 0 && settings_get("system", "peak_refresh_rate", pref, sizeof pref))
                        hz = atoi(pref);
                    if (hz > 0) {
                        int idx = mode_for_fps(hz, 1);
                        if (idx >= 0) {
                            log_msg("system default policy: set sfModeId %d (%dHz)",
                                    g_modes[idx].sf, g_modes[idx].fps);
                            set_1035(g_modes[idx].sf);
                        }
                    } else {
                        log_msg("system default policy: module idle (no prefer rate)");
                    }
                }
#endif
                log_msg("system default policy: module idle, settings restored");
                g_sys_default_restored = 1;
            }
        } else if (g_sys_default_restored) {
            g_sys_default_restored = 0;
            smart = -1;
            dirty = 1;
        }
#endif

#ifndef RRS_LEGACY_1035
        {
            static long long last_beat;
            if (last_beat == 0 || now_ms() - last_beat > 5000) {
                last_beat = now_ms();
                char b[16];
                snprintf(b, sizeof b, "%lld", (long long)time(NULL));
                settings_put("system", "rrs_beat", b);
            }
        }
#endif

        int forced = g_app_fps;
        if (forced != cur_forced) {
            cur_forced = forced;
            if (forced != 0) {
                /* 该应用单独指定刷新率：停掉游戏/智能逻辑，直接锁 */
                g_game_active = 0;
                dirty = 0;
                hold = 0;
                state = -1;
                next_down = 0;
                if (forced == -3) {

                    g_app_state = -1;
                    g_app_next_down = 0;
                } else if (forced == -2) {
#ifdef RRS_LEGACY_1035
                    apply_system_rate();
#else
                    settings_restore_auto();
                    log_msg("fps : system auto (restored)");
#endif
                } else {
                    int hz = (forced == -1) ? g_top : forced;
                    if (hz > 0)
                        apply_fps(hz, 1);   /* 前台切换立即应用，动画平滑交给动画加速 */
                }
            } else {
                smart = -1;
                dirty = 1;
                g_app_state = -1;
                g_app_next_down = 0;
                log_msg("app override end");
            }
        }

        /* 应用级智能档位：进入即最高档，之后按 smart_delays 逐级下探 */
        if (cur_forced == -3 && g_fg_ntiers > 0) {
            if (g_app_state < 0 || g_app_state >= g_fg_ntiers) {
                apply_fps(g_fg_tiers[0], 1);
                g_app_state = 0;
                g_app_next_down = now_ms() + app_delay(0);
            } else if (g_app_state < g_fg_ntiers - 1 && g_app_next_down > 0 &&
                       now_ms() >= g_app_next_down) {
                g_app_state++;
                if (g_fg_tiers[g_app_state] == TIER_AUTO) {
                    int f = content_fps_detect();
                    if (f >= 0)
                        auto_avg_push(f);
                    int avg = auto_avg_value();
                    if (avg >= 0) {
                        int hz = adaptive_refresh(snap_content_fps(avg));
                        if (hz > 0) {
                            log_msg("app auto: mode %dfps -> %dHz", avg, hz);
                            apply_fps(hz, 0);
                        }
                    }
                } else {
                    apply_fps(g_fg_tiers[g_app_state], 0);
                    if (g_fg_ntiers >= 2 &&
                        g_fg_tiers[g_fg_ntiers - 1] == TIER_AUTO &&
                        g_app_state == g_fg_ntiers - 2)
                        auto_avg_reset();   /* 落到应用的前一档，重新统计 */
                }
                g_app_next_down = (g_app_state < g_fg_ntiers - 1)
                                      ? now_ms() + app_delay(g_app_state)
                                      : 0;
            }
        }

        if (!g_no_tiers && cur_forced == 0 && g_game_front && !g_game_active) {
            if (last_game != 0 && now - last_game <= 9) {
                if (now - last_game >= 5) {
                    if (hold >= 1) {
                        g_game_active = 1;
                        g_cur_fps = -1;
                        dirty = 0;
                        hold = 0;
                        if (game_smart_mode()) {
                            { char gm[96]; fmt_game_mode(gm, sizeof gm); log_msg("game_fps_mode: %s", gm); }
                            apply_fps(g_tiers[0], 1);
                            state = 0;
                            next_down = now_ms() + g_delays[0];
                        } else {
                            next_down = 0;
                            { char gm[96]; fmt_game_mode(gm, sizeof gm); log_msg("game_fps_mode: %s", gm); }
                            apply_game_fps();
                        }
                    } else {
                        hold++;
                    }
                }
            } else {
                g_game_active = 1;
                g_cur_fps = -1;
                dirty = 0;
                hold = 0;
                if (game_smart_mode()) {
                    { char gm[96]; fmt_game_mode(gm, sizeof gm); log_msg("game_fps_mode: %s", gm); }
                    apply_fps(g_tiers[0], 1);
                    state = 0;
                    next_down = now_ms() + g_delays[0];
                } else {
                    next_down = 0;
                    { char gm[96]; fmt_game_mode(gm, sizeof gm); log_msg("game_fps_mode: %s", gm); }
                    apply_game_fps();
                }
            }
        } else if (!g_game_front && g_game_active) {
            g_game_active = 0;
            g_cur_fps = -1;
            dirty = 1;
            hold = 0;
            last_game = now;
            next_down = 0;
            log_msg("game mode off");
        } else if (!g_game_front) {
            hold = 0;
        }

        if (!g_no_tiers && cur_forced == 0 && smart == 1 &&
            state >= 0 && state < g_ntiers - 1 && next_down > 0 && now_ms() >= next_down) {
            state++;
            if (g_tiers[state] == TIER_AUTO) {
                /* 落地前补采一次：120 档 2000ms 停留也能凑够 2 个样本 */
                int f = content_fps_detect();
                if (f >= 0)
                    auto_avg_push(f);
                apply_auto_tier(0);   /* 用前档统计好的低帧缓存值 */
            } else {
                if (g_ntiers >= 2 && state == g_ntiers - 2 &&
                    g_tiers[g_ntiers - 1] == TIER_AUTO)
                    auto_avg_reset();  /* 刚落到前一档：重新统计 */
                tier_apply(state, 0);
            }
            next_down = (state < g_ntiers - 1) ? now_ms() + g_delays[state] : 0;
        }




        if (!g_game_active && cur_forced == 0 && dirty) {
            /* 只钉一次系统设置：smart=0、user/peak=最高、min=最低；之后档位全走 1035。
             * 档位列表只有一个值时就等价于“固定”，多个值就是“动态”，不再区分模式。 */
            if (smart != 1) {
                smart = 1;
                last_custom = -1;
#ifdef RRS_LEGACY_1035
                repin_settings_if_needed();
                {
                    char tstr[80];
                    tier_fmt(tstr, sizeof tstr, g_tiers, g_ntiers);
                    log_msg("global_fps_mode: %s", tstr);
                    state = 0;
                    next_down = now_ms() + g_delays[0];
                    if (g_ntiers == 1 && g_tiers[0] == TIER_AUTO) {
                        apply_fps(top_numeric_tier(), 1);   /* 固定自适应：先前档最高刷新率，统计 4 秒 */
                        g_auto_next = now_ms() + 4000;
                    } else {
                        tier_apply(0, 0);
                        if (g_tiers[0] == TIER_AUTO)
                            g_auto_next = now_ms() + 2000;
                    }
                }
#else
                if (g_no_tiers) {
                    settings_restore_auto();
                    log_msg("system dynamic: smart_tiers empty, module idle");
                } else {
                    char tstr[80];
                    tier_fmt(tstr, sizeof tstr, g_tiers, g_ntiers);
                    log_msg("global_fps_mode: %s", tstr);
                    state = 0;
                    next_down = now_ms() + g_delays[0];
                    if (g_ntiers == 1 && g_tiers[0] == TIER_AUTO) {
                        apply_fps(top_numeric_tier(), 1);   /* 固定自适应：先前档最高刷新率，统计 4 秒 */
                        g_auto_next = now_ms() + 4000;
                    } else {
                        tier_apply(0, 0);
                        if (g_tiers[0] == TIER_AUTO)
                            g_auto_next = now_ms() + 2000;
                    }
                }
#endif
            }
            dirty = 0;
        }

        if (!g_ignore_touch && g_touch_fd < 0)
            open_touch();

        struct pollfd pfd[3];
        int nfds = 0;
        int wake_rd = wake_pipe[0];
        if (!g_ignore_touch) {
            pfd[nfds].fd = g_touch_fd;
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }
        if (g_inotify_fd >= 0) {
            pfd[nfds].fd = g_inotify_fd;
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }
        if (wake_rd >= 0) {
            pfd[nfds].fd = wake_rd;
            pfd[nfds].events = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }

        /* 自适应缓存值：只在「自适应前一档」统计低帧（>60 丢弃），自适应档内不再采样 */
        /* 自适应只在「前一档」统计，自适应档内不采样、不复判 */
        {
            int pre_ctx = 0;
            if (!g_no_tiers && smart == 1 && cur_forced == 0 &&
                g_ntiers >= 2 && g_tiers[g_ntiers - 1] == TIER_AUTO &&
                state == g_ntiers - 2)
                pre_ctx = 1;
            else if (cur_forced == -3 && g_fg_ntiers >= 2 &&
                     g_fg_tiers[g_fg_ntiers - 1] == TIER_AUTO &&
                     g_app_state == g_fg_ntiers - 2)
                pre_ctx = 1;
            if (pre_ctx) {
                static char auto_last_pkg[128];
                if (strcmp(auto_last_pkg, g_fg_pkg) != 0) {
                    snprintf(auto_last_pkg, sizeof auto_last_pkg, "%s", g_fg_pkg);
                    auto_avg_reset();
                }
                if (g_auto_sample_at == 0)
                    g_auto_sample_at = now_ms() + 1000;
                if (now_ms() >= g_auto_sample_at) {
                    g_auto_sample_at = now_ms() + 1000;
                    int f = content_fps_detect();
                    if (f >= 0)
                        auto_avg_push(f);
                }
            } else {
                auto_avg_reset();
            }
        }

        if (!g_no_tiers && smart == 1 &&
            g_anim_last > 0 && now_ms() - g_anim_last < 400) {
            if (cur_forced > 0 && g_anim_saved_fps == 0) {
                g_anim_saved_fps = cur_forced;
                g_anim_restore_due = now_ms() + g_delays[0];
            }
            /* 动画加速目标：cap = 本机最高刷新率；ladder = 档位表最高档 */
            int boost_hz = top_numeric_tier();
            if (strcmp(cfg_anim_boost, "ladder") != 0) {
                int cap = effective_cap();
                if (cap > 0 && cap > boost_hz)
                    boost_hz = cap;
            }
            if (g_cur_fps != boost_hz) {
                log_msg("anim boost %dHz", boost_hz);
                apply_fps(boost_hz, 0);
            }
            state = 0;
            next_down = now_ms() + g_delays[0];
        }

        if (g_anim_saved_fps > 0 && g_anim_restore_due > 0 &&
            now_ms() >= g_anim_restore_due) {
            int hz = g_anim_saved_fps;
            g_anim_saved_fps = 0;
            g_anim_restore_due = 0;
            log_msg("anim end -> restore %dHz", hz);
            apply_fps(hz, 0);
            state = 0;
            next_down = g_ntiers > 1 ? now_ms() + g_delays[0] : 0;
        }

        g_fg_forced = cur_forced;

        /* 配置改动/热重载后立即重放当前档位 */
        if (g_low_reapply) {
            g_low_reapply = 0;
            state = 0;
            next_down = g_ntiers > 1 ? now_ms() + g_delays[0] : 0;
            int t = cur_forced > 0 ? cur_forced : (g_ntiers > 0 ? g_tiers[0] : 0);
            if (cur_forced == 0 && g_ntiers > 0 && t > g_tiers[0])
                t = g_tiers[0];
            if (t > 0)
                apply_fps(t, 0);
        }

        /* 关闭低亮度：立即恢复原档位 */
        if (cfg_bright_low <= 0 && g_low_active) {
            low_leave();
            g_low_reapply = 1;
            log_msg("low brightness off");
        } else if (g_low_active && cfg_bright_low > 0 && g_fg_forced == 0) {
            /* 上限改动：按新值重建低亮度档位（比较配置原值，避免和解析后的上限来回打架） */
            if (cfg_bright_low_cap != g_low_want_cfg) {
                low_enter(cfg_bright_low_cap);
                g_low_reapply = 1;
                log_msg("low cap changed: %dHz", g_low_cap);
            }
        }

        if (cfg_bright_low > 0 && now_ms() >= g_low_next) {
            g_low_next = now_ms() + 2000;
            int raw = read_bright_raw();
            if (raw >= 0) {
                int bpct = raw * 100 / 255;
                if (bpct <= cfg_bright_low && !g_low_active && g_fg_forced == 0) {
                    low_enter(cfg_bright_low_cap);
                    {
                        char tstr[80];
                        tier_fmt(tstr, sizeof tstr, g_tiers, g_ntiers);
                        log_msg("low brightness %d%%: ladder %s", bpct, tstr);
                    }
                    int t = cur_forced > 0 ? cur_forced : g_tiers[0];
                    if (t > g_low_cap)
                        t = g_low_cap;
                    state = 0;
                    next_down = g_ntiers > 1 ? now_ms() + g_delays[0] : 0;
                    apply_fps(t, 0);
                } else if (bpct > cfg_bright_low && g_low_active) {
                    low_leave();
                    {
                        char tstr[80];
                        tier_fmt(tstr, sizeof tstr, g_tiers, g_ntiers);
                        log_msg("brightness %d%%: ladder %s", bpct, tstr);
                    }
                    int t = cur_forced > 0 ? cur_forced : g_tiers[0];
                    state = 0;
                    next_down = g_ntiers > 1 ? now_ms() + g_delays[0] : 0;
                    apply_fps(t, 0);
                }
            }
        }

        int timeout = -1;   /* 无计时任务时阻塞等事件，不做兜底轮询 */
        /* 全局档位降档计时 */
        if (smart == 1 && state < g_ntiers - 1 && next_down > 0) {
            int rem = (int)(next_down - now_ms());
            if (rem < 50) rem = 50;
            if (rem > 1000) rem = 1000;
            timeout = rem;
        }
        if (g_anim_saved_fps > 0 && g_anim_restore_due > 0) {
            int rema = (int)(g_anim_restore_due - now_ms());
            if (rema < 50) rema = 50;
            if (rema > 1000) rema = 1000;
            if (timeout < 0 || rema < timeout)
                timeout = rema;
        }
        /* 单应用多档降档计时（之前漏了，导致应用内只停在最高档） */
        if (cur_forced == -3 && g_fg_ntiers > 0 &&
            g_app_state >= 0 && g_app_state < g_fg_ntiers - 1 && g_app_next_down > 0) {
            int rem2 = (int)(g_app_next_down - now_ms());
            if (rem2 < 50) rem2 = 50;
            if (rem2 > 1000) rem2 = 1000;
            if (timeout < 0 || rem2 < timeout)
                timeout = rem2;
        }
        {
            int pre_ctx = 0;
            if (!g_no_tiers && smart == 1 && g_ntiers >= 2 &&
                g_tiers[g_ntiers - 1] == TIER_AUTO && state == g_ntiers - 2)
                pre_ctx = 1;
            else if (cur_forced == -3 && g_fg_ntiers >= 2 &&
                     g_fg_tiers[g_fg_ntiers - 1] == TIER_AUTO &&
                     g_app_state == g_fg_ntiers - 2)
                pre_ctx = 1;
            if (pre_ctx) {
                int rema = (int)(g_auto_sample_at - now_ms());
                if (g_auto_sample_at == 0) rema = 1000;
                if (rema < 100) rema = 100;
                if (rema > 1000) rema = 1000;
                if (timeout < 0 || rema < timeout)
                    timeout = rema;
            }
        }
        if (cfg_bright_low > 0) {
            int remb = (int)(g_low_next - now_ms());
            if (remb < 50) remb = 50;
            if (remb > 2000) remb = 2000;
            if (timeout < 0 || remb < timeout)
                timeout = remb;
        }
        /* config 重载防抖也参与唤醒 */
        if (cfg_due > 0) {
            int remc = (int)(cfg_due - now_ms());
            if (remc < 50) remc = 50;
            if (remc > 1000) remc = 1000;
            if (timeout < 0 || remc < timeout)
                timeout = remc;
        }
        int ret = poll(pfd, (nfds_t)nfds, timeout);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            poll(NULL, 0, 100);
            continue;
        }

        /* ret == 0（超时）不 continue：下面的定时任务（降档截止、延迟切帧率、
         * config 防抖）都靠这次唤醒处理 */


        int touch_ready = 0, inotify_ready = 0, wake_ready = 0;
        for (int i = 0; i < nfds; i++) {
            short re = pfd[i].revents;
            if (re & POLLIN) {
                if (pfd[i].fd == g_touch_fd)
                    touch_ready = 1;
                else if (pfd[i].fd == wake_rd)
                    wake_ready = 1;
                else
                    inotify_ready = 1;
            }
            if (re & (POLLHUP | POLLNVAL)) {
                if (pfd[i].fd == g_touch_fd) {
                    close(g_touch_fd);
                    g_touch_fd = -1;
                    g_touch_path[0] = '\0';
                }
            }
        }

        if (wake_ready) {
            char wtmp[64];
            while (read(wake_rd, wtmp, sizeof wtmp) > 0)
                ;
        }

        if (inotify_ready) {
            char evbuf[4096];
            ssize_t n = read(g_inotify_fd, evbuf, sizeof evbuf);
            if (n > 0) {
                for (char *p = evbuf; p < evbuf + n; ) {
                    struct inotify_event *ev = (struct inotify_event *)p;
                    const char *name = ev->len ? ev->name : "";
                    if (strcmp(name, "config") == 0) {
                        cfg_due = now_ms() + 200;   /* 连续写文件：合并成一次重载 */
                    } else if (strncmp(name, "gamebooster.db", 14) == 0) {
                        load_gb_games();
                    } else if (strcmp(name, "AppList") == 0 || strcmp(name, "GameList") == 0) {
                        load_ext_games();
                        log_msg("app list reloaded: %d", g_ext_count);
                    }
                    p += sizeof(struct inotify_event) + ev->len;
                }
            }
        }

        if (cfg_due > 0 && now_ms() >= cfg_due) {
            cfg_due = 0;
            reload_config();
            smart = -1;
            last_custom = -1;
            state = -1;
            dirty = 1;
        }

        if (touch_ready) {
            static int touch_down;
            int act = 0;
            struct input_event ie;
            ssize_t tr;
            while ((tr = read(g_touch_fd, &ie, sizeof ie)) == (ssize_t)sizeof ie) {
                if (g_touch_contact_cap == 0) {          /* 老驱动：任意事件都算触摸 */
                    act = 1;
                    continue;
                }
                if (ie.type == EV_KEY && (ie.code == BTN_TOUCH || ie.code == BTN_STYLUS)) {
                    touch_down = ie.value ? 1 : 0;
                    act = 1;                              /* 按下/抬起都算活动 */
                } else if (ie.type == EV_ABS && ie.code == ABS_MT_TRACKING_ID) {
                    touch_down = ie.value >= 0 ? 1 : 0;
                    act = 1;
                } else if (ie.type == EV_ABS &&
                           (ie.code == ABS_MT_POSITION_X || ie.code == ABS_MT_POSITION_Y ||
                            ie.code == ABS_MT_PRESSURE)) {
                    if (touch_down)
                        act = 1;                          /* 只认真实接触中的移动，忽略 hover */
                }
            }
            if (act && !g_no_tiers && cur_forced == 0 && smart == 1) {
#ifdef RRS_LEGACY_1035
                static long long last_raise;
                if (g_cur_fps != g_tiers[0] || now_ms() - last_raise > 3000) {
                    last_raise = now_ms();
                    apply_fps(g_tiers[0], 0);
                }
                state = 0;
                next_down = now_ms() + g_delays[0];
                if (g_ntiers >= 2 && g_tiers[g_ntiers - 1] == TIER_AUTO &&
                    state == g_ntiers - 2)
                    auto_avg_reset();   /* 触摸升档后重新统计低帧 */
#else
                static long long last_raise;
                if (g_cur_fps != g_tiers[0] || now_ms() - last_raise > 3000) {
                    last_raise = now_ms();
                    apply_fps(g_tiers[0], 0);
                }
                state = 0;
                next_down = now_ms() + g_delays[0];
                if (g_ntiers >= 2 && g_tiers[g_ntiers - 1] == TIER_AUTO &&
                    state == g_ntiers - 2)
                    auto_avg_reset();   /* 触摸升档后重新统计低帧 */
#endif
            }
            if (cur_forced == -3 && g_fg_ntiers > 0) {
                if (g_cur_fps != g_fg_tiers[0])
                    apply_fps(g_fg_tiers[0], 0);
                g_app_state = 0;
                g_app_next_down = now_ms() + g_delays[0];
            }
        }
    }

    if (g_logcat_pid > 0)
        kill(g_logcat_pid, SIGTERM);
    pthread_join(th, NULL);
    restore_all();
    if (g_touch_fd >= 0)
        close(g_touch_fd);
    if (g_inotify_fd >= 0)
        close(g_inotify_fd);
    log_msg("stopped");
    if (g_log_fd >= 0) { close(g_log_fd); g_log_fd = -1; }
    return 0;
}
