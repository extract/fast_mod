#ifndef _FAST_MOD_UAPI_H
#define _FAST_MOD_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* If userspace, __user doesn't exist */
#ifndef __KERNEL__
#define __user
#endif

#ifdef __KERNEL__
#include <uapi/linux/eventpoll.h>
#else
#include <sys/epoll.h>
#endif

struct fast_wait_args {
    __s64 timeout_ms;  // -1 for infinite time
    __u32 epoll_fd;    // epoll fd to shadow
    __u32 max_events;  // return buffer max events
    struct epoll_event __user *events_buffer; // result output buffer
};

#define WR_VALUE _IOW('q', 1, int32_t *)
#define RD_VALUE _IOR('q', 2, int32_t *)
#define SLEEP_WITH_TIMEOUT _IOW('q', 3, long)
#define FAST_IOCTL_WAIT _IOWR('q', 4, struct fast_wait_args)

#endif // _FAST_MOD_UAPI_H
