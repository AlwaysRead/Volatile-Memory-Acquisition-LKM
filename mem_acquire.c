/*
* mem_acquire.c - Read only volatile memory acquisition LKM
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/timekeeping.h>
#include <linux/version.h>
#include <linux/highmem.h>
#include <linux/pfn.h>
#include <uapi/linux/ioctl.h>


#define DEVICE_NAME         "memdump"
#define CLASS_NAME          "dfir"
#define PROC_NAME           "memdump_info"
#define DRIVER_VERSION      "x.0.0"

/* Default buffer: 64MB. Override the kernel param. */
#define DEFAULT_BUFFER_SIZE (64UL * 1024 * 1024)

/* ioctl magic and commands */
#define MEMDUMP_IOC_MAGIC   'M'
#define MEMDUMP_GET_SIZE    _IOR(MEMDUMP_IOC_MAGIC, 1, unsigned long)
#define MEMDUMP_GET_TS      _IOR(MEMDUMP_IOC_MAGIC, 2, unsigned long long)
#define MEMDUMP_GET_KVER    _IOR(MEMDUMP_IOC_MAGIC, 3, char[64])
#define MEMDUMP_RESET       _IO(MEMDUMP_IOC_MAGIC,  4)
 
/* ---- Module parameters -------------------------------------------------- */
 
static ulong buffer_size = DEFAULT_BUFFER_SIZE;
module_param(buffer_size, ulong, 0444);
MODULE_PARM_DESC(buffer_size, "Acquisition buffer size in bytes (default: 64MB)");
 
static bool fill_pattern = false;
module_param(fill_pattern, bool, 0444);
MODULE_PARM_DESC(fill_pattern,
    "Fill buffer with 0x00..0xFF test pattern instead of live data (default: N)");
 
/* ---- Internal state ------------------------------------------------------ */
 
static dev_t        dev_num;
static struct cdev  mem_cdev;
static struct class *mem_class;
static struct proc_dir_entry *proc_entry;
 
static char          *kernel_buffer;
static size_t         acquired_size;
static ktime_t        acq_timestamp;
static DEFINE_MUTEX(acq_lock);
 
/* ---- Helpers ------------------------------------------------------------- */
 
/**
 * acquire_physical_memory - Walk PFNs and copy pages into kernel_buffer.
 *
 * This is a best-effort approach. Pages may be skipped if they are not
 * backed, reserved, or inaccessible (e.g. MMIO regions).
 *
 * In a real production tool, supplement with /proc/iomem parsing to
 * enumerate only RAM-backed regions.
 */
static size_t acquire_physical_memory(char *buf, size_t max_bytes)
{
    unsigned long pfn, max_pfn;
    size_t offset = 0;
    struct page *pg;
    void *vaddr;
 
    max_pfn = get_num_physpages();
 
    for (pfn = 0; pfn < max_pfn && offset + PAGE_SIZE <= max_bytes; pfn++) {
        if (!pfn_valid(pfn))
            continue;
 
        pg = pfn_to_page(pfn);
 
        /* Skip reserved / special pages to avoid hangs */
        if (PageReserved(pg))
            continue;
 
        vaddr = kmap_atomic(pg);
        memcpy(buf + offset, vaddr, PAGE_SIZE);
        kunmap_atomic(vaddr);
 
        offset += PAGE_SIZE;
    }
 
    return offset;
}
 
/**
 * fill_test_pattern - Populate buffer with 0x00-0xFF cycling pattern.
 * Useful for unit-testing the acquisition pipeline without touching
 * physical memory.
 */
static void fill_test_pattern(char *buf, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++)
        buf[i] = (char)(i & 0xFF);
}
 
/* ---- File operations ----------------------------------------------------- */
 
static int mem_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&acq_lock)) {
        pr_warn("[memdump] Device busy - acquisition already in progress\n");
        return -EBUSY;
    }
 
    pr_info("[memdump] Device opened by PID %d\n", current->pid);
    return 0;
}
 
static int mem_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&acq_lock);
    pr_info("[memdump] Device closed by PID %d\n", current->pid);
    return 0;
}
 
/**
 * mem_read - Copy kernel_buffer data to user space.
 *
 * Supports arbitrary offsets so userspace can do partial/targeted reads.
 * Uses copy_to_user for safe cross-boundary transfer.
 */
static ssize_t mem_read(struct file *file, char __user *buf,
                         size_t len, loff_t *offset)
{
    size_t to_copy;
 
    if (*offset < 0)
        return -EINVAL;
 
    if ((size_t)*offset >= acquired_size)
        return 0; /* EOF */
 
    to_copy = min(len, acquired_size - (size_t)*offset);
 
    if (copy_to_user(buf, kernel_buffer + *offset, to_copy))
        return -EFAULT;
 
    *offset += to_copy;
    return (ssize_t)to_copy;
}
 
/**
 * mem_mmap - Zero-copy interface: map kernel_buffer into user VMA.
 *
 * Avoids copy_to_user overhead for large dumps.
 * Requires kernel_buffer to be vmalloc'd (which it is).
 */
static int mem_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
 
    if (size > acquired_size)
        return -EINVAL;
 
    /* remap_vmalloc_range handles page-by-page PTE setup */
    if (remap_vmalloc_range(vma, kernel_buffer, vma->vm_pgoff)) {
        pr_err("[memdump] remap_vmalloc_range failed\n");
        return -EAGAIN;
    }
 
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
    pr_info("[memdump] mmap: %lu bytes mapped to userspace\n", size);
    return 0;
}
 
/**
 * mem_llseek - Allow seeking within the dump buffer.
 *
 * Enables forensic tools to jump directly to a region of interest
 * without re-reading from the start.
 */
static loff_t mem_llseek(struct file *file, loff_t offset, int whence)
{
    return fixed_size_llseek(file, offset, whence, (loff_t)acquired_size);
}
 
/**
 * mem_ioctl - Query metadata without parsing the data stream.
 *
 * MEMDUMP_GET_SIZE  → total acquired bytes
 * MEMDUMP_GET_TS    → acquisition timestamp (nanoseconds since boot)
 * MEMDUMP_GET_KVER  → kernel version string (up to 64 bytes)
 * MEMDUMP_RESET     → re-run acquisition (refreshes buffer)
 */
static long mem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    unsigned long long ts_ns;
    char kver[64];
 
    switch (cmd) {
    case MEMDUMP_GET_SIZE:
        if (copy_to_user((void __user *)arg, &acquired_size,
                         sizeof(acquired_size)))
            return -EFAULT;
        return 0;
 
    case MEMDUMP_GET_TS:
        ts_ns = (unsigned long long)ktime_to_ns(acq_timestamp);
        if (copy_to_user((void __user *)arg, &ts_ns, sizeof(ts_ns)))
            return -EFAULT;
        return 0;
 
    case MEMDUMP_GET_KVER:
        snprintf(kver, sizeof(kver), "%d.%d.%d",
                 (LINUX_VERSION_CODE >> 16) & 0xFF,
                 (LINUX_VERSION_CODE >> 8)  & 0xFF,
                  LINUX_VERSION_CODE        & 0xFF);
        if (copy_to_user((void __user *)arg, kver, sizeof(kver)))
            return -EFAULT;
        return 0;
 
    case MEMDUMP_RESET:
        pr_info("[memdump] Re-acquisition requested\n");
        memset(kernel_buffer, 0, buffer_size);
        acq_timestamp = ktime_get();
        if (fill_pattern)
            fill_test_pattern(kernel_buffer, buffer_size);
        else
            acquired_size = acquire_physical_memory(kernel_buffer, buffer_size);
        pr_info("[memdump] Re-acquisition complete: %zu bytes\n", acquired_size);
        return 0;
 
    default:
        return -ENOTTY;
    }
}
 
static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = mem_open,
    .release        = mem_release,
    .read           = mem_read,
    .mmap           = mem_mmap,
    .llseek         = mem_llseek,
    .unlocked_ioctl = mem_ioctl,
};
 
/* ---- /proc status entry -------------------------------------------------- */
 
static int proc_show(struct seq_file *m, void *v)
{
    seq_printf(m, "driver_version : %s\n",  DRIVER_VERSION);
    seq_printf(m, "buffer_size    : %lu\n", buffer_size);
    seq_printf(m, "acquired_bytes : %zu\n", acquired_size);
    seq_printf(m, "timestamp_ns   : %lld\n", ktime_to_ns(acq_timestamp));
    seq_printf(m, "fill_pattern   : %s\n",  fill_pattern ? "yes" : "no");
    seq_printf(m, "kernel_version : %d.%d.%d\n",
               (LINUX_VERSION_CODE >> 16) & 0xFF,
               (LINUX_VERSION_CODE >> 8)  & 0xFF,
                LINUX_VERSION_CODE        & 0xFF);
    return 0;
}
 
static int proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_show, NULL);
}
 
static const struct proc_ops proc_fops = {
    .proc_open    = proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};
 
/* ---- Module init / exit -------------------------------------------------- */
 
static int __init mem_init(void)
{
    int ret;
 
    pr_info("[memdump] Initializing v%s (buffer=%lu bytes)\n",
            DRIVER_VERSION, buffer_size);
 
    /* --- Allocate acquisition buffer (vmalloc: no contiguity requirement) */
    kernel_buffer = vmalloc(buffer_size);
    if (!kernel_buffer) {
        pr_err("[memdump] Failed to allocate %lu bytes\n", buffer_size);
        return -ENOMEM;
    }
    memset(kernel_buffer, 0, buffer_size);
 
    /* --- Perform acquisition -------------------------------------------- */
    acq_timestamp = ktime_get();
    if (fill_pattern) {
        fill_test_pattern(kernel_buffer, buffer_size);
        acquired_size = buffer_size;
        pr_info("[memdump] Test pattern written (%zu bytes)\n", acquired_size);
    } else {
        acquired_size = acquire_physical_memory(kernel_buffer, buffer_size);
        pr_info("[memdump] Physical acquisition complete: %zu bytes\n",
                acquired_size);
    }
 
    /* --- Register character device --------------------------------------- */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("[memdump] alloc_chrdev_region failed: %d\n", ret);
        goto err_vfree;
    }
 
    cdev_init(&mem_cdev, &fops);
    ret = cdev_add(&mem_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("[memdump] cdev_add failed: %d\n", ret);
        goto err_unreg;
    }
 
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    mem_class = class_create(CLASS_NAME);
#else
    mem_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(mem_class)) {
        ret = PTR_ERR(mem_class);
        pr_err("[memdump] class_create failed: %d\n", ret);
        goto err_cdev;
    }
 
    if (!device_create(mem_class, NULL, dev_num, NULL, DEVICE_NAME)) {
        ret = -ENOMEM;
        goto err_class;
    }
 
    /* --- Create /proc entry ---------------------------------------------- */
    proc_entry = proc_create(PROC_ENTRY, 0444, NULL, &proc_fops);
    if (!proc_entry)
        pr_warn("[memdump] Failed to create /proc/%s\n", PROC_ENTRY);
 
    pr_info("[memdump] Ready: /dev/%s, /proc/%s\n", DEVICE_NAME, PROC_ENTRY);
    return 0;
 
err_class:
    class_destroy(mem_class);
err_cdev:
    cdev_del(&mem_cdev);
err_unreg:
    unregister_chrdev_region(dev_num, 1);
err_vfree:
    vfree(kernel_buffer);
    return ret;
}
 
static void __exit mem_exit(void)
{
    if (proc_entry)
        remove_proc_entry(PROC_ENTRY, NULL);
 
    device_destroy(mem_class, dev_num);
    class_destroy(mem_class);
    cdev_del(&mem_cdev);
    unregister_chrdev_region(dev_num, 1);
    vfree(kernel_buffer);
 
    pr_info("[memdump] Module unloaded\n");
}
 
module_init(mem_init);
module_exit(mem_exit);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cybersecurity Research");
MODULE_DESCRIPTION("Read-only volatile memory acquisition module for DFIR");
MODULE_VERSION(DRIVER_VERSION);
 

