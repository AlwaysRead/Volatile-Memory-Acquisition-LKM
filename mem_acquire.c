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

/* ioctl defination */
#define MEMDUMP_IOC_MAGIC   'M'
#define MEMDUMP_GET_SIZE    

