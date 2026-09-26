
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/utsname.h>
#include <linux/mm.h>
#include <linux/sysinfo.h>
#include <linux/sched/signal.h>
#include <linux/timekeeping.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "kfetch.h"   

/* 全域鎖，確保 thread-safe */
static DEFINE_MUTEX(kfetch_lock);

/* 目前要顯示哪些資訊的 mask，預設顯示全部 */
static int info_mask = KFETCH_FULL_INFO;

/* 把 pages 轉成 MB（2^20 bytes） */
static unsigned long pages_to_mb(unsigned long pages)
{
    return (pages << (PAGE_SHIFT - 20));
}

/* Hostname：必顯示 */
static const char *get_hostname(void)
{
    return init_uts_ns.name.nodename;
}

/* Kernel release */
static const char *get_release(void)
{
    return init_uts_ns.name.release;
}

/* CPUs: online / total */
static void get_cpu_nums(int *online, int *total)
{
    *online = num_online_cpus();
    *total  = num_possible_cpus();
}


static const char *get_cpu_model(void)
{
    return init_uts_ns.name.machine;
}

/* Mem: free / total (MB) */
static void get_meminfo(unsigned long *free_mb, unsigned long *total_mb)
{
    struct sysinfo si;

    si_meminfo(&si);
    *total_mb = pages_to_mb(si.totalram);
    *free_mb  = pages_to_mb(si.freeram);
}

/* Uptime：開機到現在幾分鐘 */
static unsigned long get_uptime_minutes(void)
{
    u64 sec = ktime_get_boottime_seconds();
    return div_u64(sec, 60);
}

/* Procs：目前系統中 process 個數 */
static unsigned long get_num_procs(void)
{
    struct task_struct *p;
    unsigned long count = 0;

    rcu_read_lock();
    for_each_process(p)
        count++;
    rcu_read_unlock();

    return count;
}

/*
 * 組出整個輸出字串，寫到 buf 裡。
 * 回傳實際使用的長度。
 */
/* 組出整個輸出字串，左邊 logo，右邊資訊 (neofetch style) */
static size_t build_output(char *buf, size_t size)
{
    size_t len = 0;
    int i;
    /* 左邊的 ASCII logo（7 行） */
    static const char *logo[] = {
        "        .-.       ",
        "       (.. |      ",
        "       <>  |      ",
        "      / --- \\     ",
        "     ( |   | )    ",
        "   |\\\\_)__(_//|  ",
        "  <__)------(__>  "
    };
    const int logo_lines = ARRAY_SIZE(logo);

    /* 右邊的文字資訊，每行一條 */
    char info[16][80];   /* 最多 16 行，每行最多 79 字 + '\0' */
    int info_lines = 0;

    const char *hostname = get_hostname();
    size_t hostlen = strlen(hostname);

    int online_cpus, total_cpus;
    unsigned long free_mb, total_mb;
    unsigned long uptime_min;
    unsigned long num_procs;

    /* ===== 先把右邊要顯示的資訊逐行填進 info[] ===== */

    /* 1. Hostname（必顯示） */
    scnprintf(info[info_lines++], sizeof(info[0]), "%s", hostname);

    /* 2. 分隔線（長度同 hostname） */
    scnprintf(info[info_lines++], sizeof(info[0]),
              "%.*s", (int)hostlen,
              "------------------------------------------------------------");

    /* 3. 依照 mask 加上各項資訊 */
    if (info_mask & KFETCH_RELEASE) {
        scnprintf(info[info_lines++], sizeof(info[0]),
                  "Kernel: %s", get_release());
    }

    if (info_mask & KFETCH_CPU_MODEL) {
        scnprintf(info[info_lines++], sizeof(info[0]),
                  "CPU:    %s", get_cpu_model());
    }

    if (info_mask & KFETCH_NUM_CPUS) {
        get_cpu_nums(&online_cpus, &total_cpus);
        scnprintf(info[info_lines++], sizeof(info[0]),
                  "CPUs:   %d / %d", online_cpus, total_cpus);
    }

    if (info_mask & KFETCH_MEM) {
        get_meminfo(&free_mb, &total_mb);
        scnprintf(info[info_lines++], sizeof(info[0]),
                  "Mem:    %lu MB / %lu MB",
                  free_mb, total_mb);
    }

    if (info_mask & KFETCH_NUM_PROCS) {
        num_procs = get_num_procs();
        scnprintf(info[info_lines++], sizeof(info[0]),
                  "Procs:  %lu", num_procs);
    }

    if (info_mask & KFETCH_UPTIME) {
        uptime_min = get_uptime_minutes();
        scnprintf(info[info_lines++], sizeof(info[0]),
                  "Uptime: %lu mins", uptime_min);
    }

    /* ===== 把左邊 logo + 右邊 info 合併成每一行 ===== */

    {
        int max_lines = (logo_lines > info_lines) ? logo_lines : info_lines;

        for (i = 0; i < max_lines && len < size; i++) {
            const char *left  = (i < logo_lines) ? logo[i]     : "";
            const char *right = (i < info_lines) ? info[i] : "";

            /* %-22s 左欄寬度固定 22 字元，右邊接上資訊 */
            len += scnprintf(buf + len, size - len,
                             "%-22s %s\n", left, right);
        }
    }

    return len;
}


/* open：這裡只設定成 non-seekable 即可 */
static int kfetch_open(struct inode *inode, struct file *filp)
{
    nonseekable_open(inode, filp);
    return 0;
}

/* release：目前沒特別要清理的 per-file 資源 */
static int kfetch_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * write：從 user-space 讀入一個 int 當作 mask
 * user 端會用：
 *   int mask; write(fd, &mask, sizeof(mask));
 */
static ssize_t kfetch_write(struct file *filp,
                            const char __user *buffer,
                            size_t length,
                            loff_t *offset)
{
    int mask;

    if (length < sizeof(int))
        return -EINVAL;

    if (copy_from_user(&mask, buffer, sizeof(int)))
        return -EFAULT;

    mutex_lock(&kfetch_lock);
    info_mask = mask;
    mutex_unlock(&kfetch_lock);

    return sizeof(int);
}

/*
 * read：一次把整塊資訊塞回去。
 * 這裡採「讀一次就 EOF」：
 *   - 第一次 read：*offset == 0 → 回傳資料並更新 offset
 *   - 再次 read：*offset != 0 → 回傳 0
 */
static ssize_t kfetch_read(struct file *filp,
                           char __user *buffer,
                           size_t length,
                           loff_t *offset)
{
    char *kbuf;
    size_t out_len;
    ssize_t ret;

    /* 只允許讀一次，之後直接 EOF */
    if (*offset != 0)
        return 0;

    kbuf = kmalloc(KFETCH_BUF_SIZE, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    mutex_lock(&kfetch_lock);
    out_len = build_output(kbuf, KFETCH_BUF_SIZE);
    mutex_unlock(&kfetch_lock);

    /* 避免超過 user 提供的長度 */
    if (out_len > length)
        out_len = length;

    if (copy_to_user(buffer, kbuf, out_len)) {
        ret = -EFAULT;
        goto out;
    }

    *offset += out_len;
    ret = out_len;

out:
    kfree(kbuf);
    return ret;
}

/* file_operations */
static const struct file_operations kfetch_fops = {
    .owner   = THIS_MODULE,
    .open    = kfetch_open,
    .release = kfetch_release,
    .read    = kfetch_read,
    .write   = kfetch_write,
};

/*
 * 使用 miscdevice，讓 kernel 自動幫你建立 /dev/kfetch
 */
static struct miscdevice kfetch_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = KFETCH_DEV_NAME,   // "kfetch"
    .fops  = &kfetch_fops,
    .mode  = 0666,              // 一般使用者也可以讀寫
};

static int __init kfetch_init(void)
{
    int ret;

    mutex_lock(&kfetch_lock);
    info_mask = KFETCH_FULL_INFO;  // module 載入時預設顯示全部
    mutex_unlock(&kfetch_lock);

    ret = misc_register(&kfetch_miscdev);
    if (ret) {
        pr_err("kfetch_mod_A141064: failed to register misc device\n");
        return ret;
    }

    pr_info("kfetch_mod_A141064: module loaded\n");
    return 0;
}

static void __exit kfetch_exit(void)
{
    misc_deregister(&kfetch_miscdev);
    pr_info("kfetch_mod_A141064: module unloaded\n");
}

module_init(kfetch_init);
module_exit(kfetch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("A141064");
MODULE_DESCRIPTION("kfetch system information module for /dev/kfetch");
