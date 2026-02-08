#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/list.h>
#include "fast_mod_uapi.h"
#include "shadow_structs.h"

#define DEVICE_NAME "fast_ioctl_dev"
#define CLASS_NAME  "fast_mod"

static dev_t dev_num;
static struct class *my_class = NULL;
static int32_t value = 0;

struct fast_device {
    int32_t internal_value;
    wait_queue_head_t queue; // The "bench" lives here
    struct cdev cdev;
};

static int drain_rdllist(struct eventpoll_shadow *ep, struct fast_wait_args *args) {
    struct epitem_shadow *epi, *tmp;
    int count = 0;
    unsigned long flags;
    struct list_head local_list;

    INIT_LIST_HEAD(&local_list);

    // 1. LOCK & MOVE: Minimal time spent with IRQs disabled
    spin_lock_irqsave(&ep->lock, flags);
    
    // Move all entries from rdllist to our local list
    list_splice_init(&ep->rdllist, &local_list);
    
    spin_unlock_irqrestore(&ep->lock, flags);

    // 2. PROCESS: Now we can safely call copy_to_user (we aren't holding a lock!)
    list_for_each_entry_safe(epi, tmp, &local_list, rdllink) {
        if (count >= args->max_events) {
            // If we hit the user's limit, put the rest back on the main list
            spin_lock_irqsave(&ep->lock, flags);
            list_splice(&local_list, &ep->rdllist);
            spin_unlock_irqrestore(&ep->lock, flags);
            break;
        }

        if (copy_to_user(&args->events_buffer[count], &epi->event, sizeof(struct epoll_event))) {
            return -EFAULT;
        }

        // Successfully copied, remove from our local list
        list_del_init(&epi->rdllink);
        count++;
    }

    return count;
}

static long fast_epoll_wait(struct fast_device *dev, struct fast_wait_args *args) {
    struct fd f;
    struct eventpoll_shadow *ep;
    int event_count = 0;
    long timeout_jiffies = MAX_SCHEDULE_TIMEOUT;
   	if (args->timeout_ms >= 0) {
   	    timeout_jiffies = msecs_to_jiffies(args->timeout_ms);
    }
    // 1. Get the file structure from the FD
    f = fdget(args->epoll_fd);
    struct file *file_ptr = fd_file(f);
    if (!file_ptr) return -EBADF;

    // 2. Validate it's actually an epoll file
    // (Epoll files usually have a specific f_op pointer)
    
    // 3. Extract the internal eventpoll pointer
    // Epoll stores its private data in f.file->private_data
    ep = (struct eventpoll_shadow *)file_ptr->private_data;

    // 4. THE FAST PATH: Check the ready list
    // We need a lock here because the kernel might be adding events 
    // to this list from an interrupt or another core.
    
    /* Note: You'll need to find the spinlock offset in struct eventpoll too */
    
    if (list_empty(&ep->rdllist)) {
        // spin_unlock_irqrestore(&ep->lock, flags);
        // long ret = wait_event_interruptible_timeout(ep->wq, !list_empty(&ep->rdllist), timeout_jiffies);
        // 5. If empty, sleep on epoll's own wait queue
        // This is the "secret sauce": we use their queue, but our logic
        pr_info("FastMod: Epoll list empty, sleeping on epoll wq for %lld ms...\n", args->timeout_ms);
        long ret = wait_event_interruptible_timeout(ep->wq, !list_empty(&ep->rdllist), timeout_jiffies);
        if (ret < 0) {
            fdput(f);
            return -ERESTARTSYS;
        } else if (ret == 0) {
            fdput(f);
            return 0;
        }
    }

    // 6. When we wake up, rdllist is guaranteed to have data
    // You would then iterate rdllist and copy data to userspace
    event_count = drain_rdllist(ep, args);
    fdput(f);
    return event_count;
}

// fs.h 1527     long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
// This function is triggered when userspace calls ioctl()
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct fast_wait_args args;
    struct fast_device *dev = (struct fast_device *)file->private_data;

    switch(cmd) {
        case WR_VALUE:
            if (copy_from_user(&value, (int32_t*) arg, sizeof(value))) {
                pr_err("FastMod: Data Write Error\n");
            }
            pr_info("FastMod: Value set to %d\n", value);
            break;
        case RD_VALUE:
            if (copy_to_user((int32_t*) arg, &value, sizeof(value))) {
                pr_err("FastMod: Data Read Error\n");
            }
            break;
    	case FAST_IOCTL_WAIT:
            if (copy_from_user(&args, (struct fast_wait_args __user *)arg, sizeof(args))) {
                return -EFAULT;
            }
            pr_info("FastMod: Epoll_wait\n");
	    	return fast_epoll_wait(dev, &args);
	        break;
        default:
            pr_info("FastMod: Default operation\n");
            break;
    }
    return 0;
}

static int device_open(struct inode *inode, struct file *file) {
    struct fast_device *dev;
    // Offset arithmetic magic
    dev = container_of(inode->i_cdev, struct fast_device, cdev);
    file->private_data = dev;
    return 0;
}

static __poll_t device_poll(struct file *file, poll_table *wait) {
    struct fast_device *dev = file->private_data;
    __poll_t mask = 0;

    // This doesn't sleep! It just registers the 'wait' table
    // (which epoll provides) into our driver's wait_queue.
    poll_wait(file, &dev->queue, wait);

    // If our condition is met, tell epoll we are ready to be read
    if (dev->internal_value > 0) {
        mask |= (EPOLLIN | EPOLLRDNORM);
    }

    return mask;
}

static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
    .open           = device_open,
    .poll           = device_poll,
};

static int __init fast_mod_init(void) {
    struct fast_device *my_fast_device = kzalloc(sizeof(struct fast_device), GFP_KERNEL);
    if (!my_fast_device) {
        return -ENOMEM;
    }

    // 1. Allocate major number
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) return -1;

    // 2. Create device class
    my_class = class_create(CLASS_NAME);

    // 3. Create device node in /dev/
    struct device *my_device = device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME);

    // 4. Initialize cdev structure
    init_waitqueue_head(&my_fast_device->queue);
    cdev_init(&my_fast_device->cdev, &fops);
    my_fast_device->cdev.owner = THIS_MODULE;
    int errorCode = cdev_add(&my_fast_device->cdev, dev_num, 1);
    if (errorCode < 0) {
        // Cleanup on failure
        device_destroy(my_class, dev_num);
        class_destroy(my_class);
        unregister_chrdev_region(dev_num, 1);
        kfree(my_fast_device);
        pr_err("FastMod: Failure to add cdev errorCode %d, cleaned up\n", errorCode);
        return -1;
    }
    // Storing the driver data so we can clean up in exit
    dev_set_drvdata(my_device, my_fast_device);

    pr_info("FastMod: Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit fast_mod_exit(void) {
    struct device *my_device;
    struct fast_device *my_fast_device;
    my_device = class_find_device_by_devt(my_class, dev_num);
    if (my_device) {
        my_fast_device = dev_get_drvdata(my_device);
        put_device(my_device); // Release the reference from find_device
    }

    // 1. Destroy visible device node first
    device_destroy(my_class, dev_num);
    
    // 2. Destroy the class
    class_destroy(my_class);

    // 3. Unregister the region
    unregister_chrdev_region(dev_num, 1);

    // 4. Finally, free the memory
    if (my_fast_device) {
        cdev_del(&my_fast_device->cdev);
        kfree(my_fast_device);
    }
    pr_info("FastMod: Module unloaded\n");
}

module_init(fast_mod_init);
module_exit(fast_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AI Partner");
MODULE_DESCRIPTION("A fast IOCTL-driven kernel module");
