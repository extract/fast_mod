#ifndef _FAST_MOD_UAPI_H
#define _FAST_MOD_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* If we are in userspace, __user doesn't exist, so we define it as nothing */
#ifndef __KERNEL__
#define __user
#endif
/* Standard epoll header for userspace */
#ifdef __KERNEL__
#include <uapi/linux/eventpoll.h>
#else
#include <sys/epoll.h>
#endif

struct fast_wait_args {
    __s64 timeout_ms;  /* Signed so we can pass -1 for infinity */
    __u32 epoll_fd;    /* The FD we are shadowing */
    __u32 max_events;                     /* How many events can the buffer hold? */
    struct epoll_event __user *events_buffer; /* Where to copy the results */
};

#define WR_VALUE _IOW('q', 1, int32_t *)
#define RD_VALUE _IOR('q', 2, int32_t *)
#define SLEEP_WITH_TIMEOUT _IOW('q', 3, long)
#define FAST_IOCTL_WAIT _IOWR('q', 4, struct fast_wait_args)

#endif // _FAST_MOD_UAPI_H
