#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <errno.h>

#include "fast_mod_uapi.h"

struct thread_context {
    int pipe_write_fd;
    long timeout_usec;
};

typedef struct {
    int read;
    int write;
} pipe_t;

pipe_t create_signaling_pipe(void) {
    int fds[2];
    if (pipe(fds) < 0) {
        perror("Pipe creation failed");
        exit(EXIT_FAILURE);
    }
    return (pipe_t){ .read = fds[0], .write = fds[1] };
}

int create_fast_epoll() {
    int ep_fd = epoll_create1(0);
    if (ep_fd < 0) {
        perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }

    return ep_fd;
}

void setup_epoll_monitoring(int ep_fd, int target_fd) {
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.fd = target_fd
    };

    if (epoll_ctl(ep_fd, EPOLL_CTL_ADD, target_fd, &ev) < 0) {
        perror("epoll_ctl: ADD failed");
        exit(EXIT_FAILURE);
    }
}

void* producer_thread(void* arg) {
    struct thread_context* ctx = (struct thread_context*)arg;
    
    printf("[Producer] Sleeping for %f seconds (simulating delay)...\n", (ctx->timeout_usec / (float)1'000'000));
    usleep(ctx->timeout_usec);

    printf("[Producer] Writing to pipe...\n");
    uint64_t val = 1;
    if (write(ctx->pipe_write_fd, &val, sizeof(val)) < 0) {
        perror("Producer write failed");
    }
    return NULL;
}

void create_thread(pthread_t *thread_id, struct thread_context *ctx) {
    if (pthread_create(thread_id, NULL, producer_thread, ctx) != 0) {
        perror("pthread_create failed");
        exit(1);
    }
}

int main() {
    pthread_t thread_id;
    struct epoll_event events_out[10];

    pipe_t signaling = create_signaling_pipe();
    int ep_fd = create_fast_epoll();

    setup_epoll_monitoring(ep_fd, signaling.read);

    struct thread_context ctx = {
        .pipe_write_fd = signaling.write,
        .timeout_usec = 1 * 1'000'000
    };
    create_thread(&thread_id, &ctx);

    int dev_fd = open("/dev/fast_ioctl_dev", O_RDWR);
    if (dev_fd < 0) { perror("Device open failed"); return 1; }

    struct fast_wait_args args = {
        .timeout_ms = 5000,
        .epoll_fd = ep_fd,
        .max_events = 10,
        .events_buffer = events_out
    };

    printf("[Main] Entering Fast IOCTL wait...\n");
    int ret = ioctl(dev_fd, FAST_IOCTL_WAIT, &args);

    if (ret < 0) {
        perror("IOCTL failed");
    } else if (ret == 0) {
        printf("[Main] Timeout reached.\n");
    } else {
        printf("[Main] SUCCESS! Woke up with %d event(s).\n", ret);
        uint64_t dummy;
        // neeed to read the fd otherwise it cant be closed
        read(signaling.read, &dummy, sizeof(dummy));
    }

    // clean up
    pthread_join(thread_id, NULL);
    close(signaling.read); close(signaling.write);
    close(ep_fd); close(dev_fd);
    
    return 0;
}
