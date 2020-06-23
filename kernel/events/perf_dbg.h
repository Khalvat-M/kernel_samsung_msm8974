#ifndef	__PERF_DBG_H__
#define __PERF_DBG_H__
#define DEBUG

#define PERF_DEBUG
#ifdef	PERF_DEBUG
#define perf_debug(fmt, ...) \
        printk(KERN_DEBUG "%s:%d:"pr_fmt(fmt), \
                        __func__, __LINE__, ##__VA_ARGS__)
#else
#define perf_debug(fmt, ...) \
        no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#endif
#endif
