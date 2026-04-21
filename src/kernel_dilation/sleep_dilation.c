#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/hrtimer.h>
#include <linux/math.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A module to dilate sleep timers");
MODULE_VERSION("0.5");

// Dilation factor is stored as "parts per thousand". 1000 = 1.0x, 1500 = 1.5x, etc.
static unsigned int dilation_factor = 1000;
static const unsigned int DILATION_DENOMINATOR = 1000;

static int target_pid = 0; // 0 means affect all processes

// --- Sysfs setup ---

static struct kobject *sleep_dilation_kobj;

// 'show'/'store' for dilation_factor
static ssize_t dilation_factor_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%u/%u\n", dilation_factor, DILATION_DENOMINATOR);
}

static ssize_t dilation_factor_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    unsigned int val;
    int res = kstrtouint(buf, 10, &val);
    if (res < 0)
        return res;

    dilation_factor = val;
    pr_info("dilation_factor set to %u/%u\n", dilation_factor, DILATION_DENOMINATOR);
    return count;
}

// 'show'/'store' for target_pid
static ssize_t target_pid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", target_pid);
}

static ssize_t target_pid_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    int res = kstrtoint(buf, 10, &target_pid);
    if (res < 0)
        return res;

    pr_info("target_pid set to %d\n", target_pid);
    return count;
}

// Define the sysfs attributes
static struct kobj_attribute dilation_factor_attr = __ATTR(dilation_factor, 0664, dilation_factor_show, dilation_factor_store);
static struct kobj_attribute target_pid_attr = __ATTR(target_pid, 0664, target_pid_show, target_pid_store);

static struct attribute *attrs[] = {
    &dilation_factor_attr.attr,
    &target_pid_attr.attr,
    NULL, // Must be NULL-terminated
};

static struct attribute_group attr_group = {
  .attrs = attrs,
};


// --- kprobe for schedule_timeout ---

static int handler_pre_sched_timeout(struct kprobe *p, struct pt_regs *regs)
{
    long timeout = (long)regs->di;
    unsigned int curr_state = get_current_state();

    // Safety Check 1: Ignore indefinite sleeps.
    if (timeout == MAX_SCHEDULE_TIMEOUT) {
        return 0;
    }

    // Safety Check 2: Only modify interruptible sleeps.
    if (!(curr_state & TASK_INTERRUPTIBLE)) {
        return 0;
    }
    
    // Optimization: Don't do anything if factor is 1.0
    if (dilation_factor == DILATION_DENOMINATOR) {
        return 0;
    }

    // Ignore non-positive timeouts which are not MAX_SCHEDULE_TIMEOUT
    if (timeout <= 0) {
        return 0;
    }

    long new_timeout = mult_frac(timeout, dilation_factor, DILATION_DENOMINATOR);
    pr_info("Dilating schedule_timeout for %s (PID %d) from %ld to %ld jiffies\n",
            current->comm, current->pid, timeout, new_timeout);
    regs->di = new_timeout;

    return 0;
}

static struct kprobe kp_sched_timeout = {
.symbol_name = "schedule_timeout",
.pre_handler = handler_pre_sched_timeout,
};

// --- kprobe for hrtimer_nanosleep ---

static int handler_pre_hrtimer_nanosleep(struct kprobe *p, struct pt_regs *regs)
{
    // Filter by target PID if one is set
    if (target_pid > 0 && current->tgid!= target_pid) {
        return 0;
    }

    // Optimization: Don't do anything if factor is 1.0
    if (dilation_factor == DILATION_DENOMINATOR) {
        return 0;
    }

    ktime_t rqtp = (ktime_t)regs->di;
    
    // Ignore non-positive sleep requests
    if (rqtp <= 0) {
        return 0;
    }

    ktime_t new_rqtp = mult_frac(rqtp, dilation_factor, DILATION_DENOMINATOR);

    pr_info("Dilating hrtimer_nanosleep for %s (PID %d) from %lld ns to %lld ns\n",
            current->comm, current->pid, (long long)rqtp, (long long)new_rqtp);

    regs->di = (unsigned long)new_rqtp;

    return 0;
}

static struct kprobe kp_hrtimer_nanosleep = {
.symbol_name = "hrtimer_nanosleep",
.pre_handler = handler_pre_hrtimer_nanosleep,
};

// --- Module Init and Exit ---

static int __init sleep_dilation_init(void)
{
    int ret;

    pr_info("Sleep Dilation Module: Initializing\n");

    sleep_dilation_kobj = kobject_create_and_add("sleep_dilation", kernel_kobj);
    if (!sleep_dilation_kobj) {
        pr_err("Failed to create kobject\n");
        return -ENOMEM;
    }

    ret = sysfs_create_group(sleep_dilation_kobj, &attr_group);
    if (ret) {
        pr_err("Failed to create sysfs group\n");
        kobject_put(sleep_dilation_kobj);
        return ret;
    }

    ret = register_kprobe(&kp_sched_timeout);
    if (ret < 0) {
        pr_err("register_kprobe for schedule_timeout failed, returned %d\n", ret);
        sysfs_remove_group(sleep_dilation_kobj, &attr_group);
        kobject_put(sleep_dilation_kobj);
        return ret;
    }
    pr_info("Planted kprobe at %p\n", kp_sched_timeout.addr);

    ret = register_kprobe(&kp_hrtimer_nanosleep);
    if (ret < 0) {
        pr_err("register_kprobe for hrtimer_nanosleep failed, returned %d\n", ret);
        unregister_kprobe(&kp_sched_timeout);
        sysfs_remove_group(sleep_dilation_kobj, &attr_group);
        kobject_put(sleep_dilation_kobj);
        return ret;
    }
    pr_info("Planted kprobe at %p\n", kp_hrtimer_nanosleep.addr);

    return 0;
}

static void __exit sleep_dilation_exit(void)
{
    unregister_kprobe(&kp_hrtimer_nanosleep);
    pr_info("kprobe hrtimer_nanosleep unregistered\n");

    unregister_kprobe(&kp_sched_timeout);
    pr_info("kprobe schedule_timeout unregistered\n");

    sysfs_remove_group(sleep_dilation_kobj, &attr_group);
    kobject_put(sleep_dilation_kobj);
    pr_info("Sysfs components removed\n");

    pr_info("Sleep Dilation Module: Exiting\n");
}

module_init(sleep_dilation_init);
module_exit(sleep_dilation_exit);