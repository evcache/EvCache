#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/vcolor.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/ctype.h>

#define MiB (1 << 20)

static struct proc_dir_entry *proc_entry;

static ssize_t vcolor_proc_read(struct file *f, char __user *buf,
                               size_t count, loff_t *ppos)
{
    char kbuf[1024];
    int len = 0;
    size_t n_tot_colored = 0, n_tot_alloced = 0;

    len += scnprintf(kbuf + len, sizeof(kbuf) - len,
                     "Status: %s\n"
                     "--------------------------------------------\n"
                     "Color        Free           Allocated\n"
                     "--------------------------------------------\n",
                     vcolor_enabled ? "ENABLED" : "DISABLED");

    spin_lock(&vcolor_lock);
    for (int i = 0; i < VCOLOR_MAX_COLORS; i++) {
        len += scnprintf(kbuf + len, sizeof(kbuf) - len,
                         "%2d       %6lu            %6lu\n",
                         i, colored_count[i], allocated_count[i]);
        n_tot_colored += colored_count[i];
        n_tot_alloced += allocated_count[i];
    }

    len += scnprintf(kbuf + len, sizeof(kbuf) - len,
                     "Total    %6lu (%4lu MiB) %6lu (%4lu MiB)\n",
		     n_tot_colored, n_tot_colored * PAGE_SIZE / MiB, n_tot_alloced, n_tot_alloced * PAGE_SIZE / MiB);

    len += scnprintf(kbuf + len, sizeof(kbuf) - len,
                        "--------------------------------------------\n"
                        "last_alloc: %s\n", last_alloc_comm);
    len += scnprintf(kbuf + len, sizeof(kbuf) - len,
                     "last_writer: %s\n", writer_comm);
    len += scnprintf(kbuf + len, sizeof(kbuf) - len,
                     "hottest: %u\norder:", vcolor_hottest);
    for (int i = 0; i < VCOLOR_MAX_COLORS; i++)
        len += scnprintf(kbuf + len, sizeof(kbuf) - len, " %u", vcolor_order[i]);
    len += scnprintf(kbuf + len, sizeof(kbuf) - len, "\n");
    spin_unlock(&vcolor_lock);

    return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t vcolor_proc_write(struct file *f, const char __user *buf,
                                 size_t count, loff_t *ppos)
{
    char kbuf[64];
    unsigned long pfn;
    unsigned int color;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = '\0';

    strscpy(writer_comm, current->comm, TASK_COMM_LEN);

    if (sysfs_streq(kbuf, "enable")) {
        vcolor_enabled = true;
        return count;
    }
    if (sysfs_streq(kbuf, "disable")) {
        vcolor_flush();
        return count;
    }
    if (sysfs_streq(kbuf, "flush") || sysfs_streq(kbuf, "clear")) {
        vcolor_flush();
        vcolor_enabled = true;
        return count;
    }

    if (strncmp(kbuf, "order", 5) == 0) {
        bool used[VCOLOR_MAX_COLORS] = { false };
        int idx = 0;
        char *p = kbuf + 5;
        unsigned long val;
        while (idx < VCOLOR_MAX_COLORS) {
            while (*p && isspace(*p))
                p++;
            if (!*p)
                break;
            if (kstrtoul(p, 0, &val))
                return -EINVAL;
            if (val >= VCOLOR_MAX_COLORS)
                return -EINVAL;
            vcolor_order[idx++] = (u8)val;
            used[val] = true;
            while (*p && !isspace(*p))
                p++;
        }
        for (int i = 0; i < VCOLOR_MAX_COLORS && idx < VCOLOR_MAX_COLORS; i++)
            if (!used[i])
                vcolor_order[idx++] = i;
        vcolor_hottest = vcolor_order[0];
        return count;
    }

    if (sscanf(kbuf, "hot %u", &color) == 1) {
        if (color < VCOLOR_MAX_COLORS) {
            int pos = -1;
            for (int i = 0; i < VCOLOR_MAX_COLORS; i++)
                if (vcolor_order[i] == color) {
                    pos = i; break; }
            if (pos > 0) {
                memmove(&vcolor_order[1], &vcolor_order[0], pos * sizeof(u8));
                vcolor_order[0] = color;
            } else if (pos == -1) {
                memmove(&vcolor_order[1], &vcolor_order[0],
                        (VCOLOR_MAX_COLORS-1) * sizeof(u8));
                vcolor_order[0] = color;
            }
            vcolor_hottest = color;
        }
        return count;
    }

    if (sscanf(kbuf, "free %u", &color) == 1) {
        if (color < VCOLOR_MAX_COLORS)
            vcolor_release_color(color);
        return count;
    }

    if (sscanf(kbuf, "%lx %u", &pfn, &color) == 2) {
        struct page *page = pfn_to_page(pfn);
        if (color < VCOLOR_MAX_COLORS)
            vcolor_tag_frame(page, color);
        return count;
    }
    return -EINVAL;
}

static const struct proc_ops vcolor_proc_ops = {
    .proc_read  = vcolor_proc_read,
    .proc_write = vcolor_proc_write,
};

static int __init vcolor_init(void)
{
    proc_entry = proc_create("vcolor_km", 0666, NULL, &vcolor_proc_ops);
    if (!proc_entry)
        return -ENOMEM;
    vcolor_enabled = false;
    return 0;
}
module_init(vcolor_init);

static void __exit vcolor_exit(void)
{
    vcolor_flush();
    proc_remove(proc_entry);
}
module_exit(vcolor_exit);

MODULE_LICENSE("GPL");
