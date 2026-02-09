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
    wait_queue_head_t queue;
    struct cdev cdev;
};

static int drain_rdllist(struct eventpoll_shadow *ep, struct fast_wait_args *args) {
    struct epitem_shadow *epi, *tmp;
    int count = 0;
    unsigned long flags;
    struct list_head local_list;

    INIT_LIST_HEAD(&local_list);

    // lock ep
    spin_lock_irqsave(&ep->lock, flags);
    
    // combine and move all entries from rdllist to local_list
    // plan is to do a bulk move for max_events later
    list_splice_init(&ep->rdllist, &local_list);
    
    // unlock ep
    spin_unlock_irqrestore(&ep->lock, flags);

    // note: we're the lockless monster :)
    list_for_each_entry_safe(epi, tmp, &local_list, rdllink) {
        // we can only process as many items as mentioned in args->max_events
        if (count >= args->max_events) {
            // lock ep
            spin_lock_irqsave(&ep->lock, flags);
            // thsi time we move the remaining items into the rdllist
            list_splice(&local_list, &ep->rdllist);
            // unlock ep
            spin_unlock_irqrestore(&ep->lock, flags);
            break;
        }

        // now we do the bulk move of the items
        if (copy_to_user(&args->events_buffer[count], &epi->event, sizeof(struct epoll_event))) {
            return -EFAULT;
        }

        // now we need to remove it from the ready list
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
    f = fdget(args->epoll_fd);
    struct file *file_ptr = fd_file(f);
    if (!file_ptr) return -EBADF;

    ep = (struct eventpoll_shadow *)file_ptr->private_data;

    if (list_empty(&ep->rdllist)) {
        pr_info("sleeping on epoll wq for %lld ms...\n", args->timeout_ms);
        long ret = wait_event_interruptible_timeout(ep->wq, !list_empty(&ep->rdllist), timeout_jiffies);
        if (ret < 0) {
            fdput(f);
            return -ERESTARTSYS;
        } else if (ret == 0) {
            fdput(f);
            return 0;
        }
    }

    event_count = drain_rdllist(ep, args);
    fdput(f);
    return event_count;
}

// fs.h 1527     long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
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
    // container_of gets the struct of fast_device based on cdev... compile time magic
    // we could have used this to get the unwrap
    dev = container_of(inode->i_cdev, struct fast_device, cdev);
    file->private_data = dev;
    return 0;
}

static __poll_t device_poll(struct file *file, poll_table *wait) {
    struct fast_device *dev = file->private_data;
    __poll_t mask = 0;

    // this doesn't sleep! it just registers the 'wait' table
    // (which epoll provides) into our driver's wait_queue.
    poll_wait(file, &dev->queue, wait);

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

    // get number to device
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) return -1;

    // create class (for udev)
    my_class = class_create(CLASS_NAME);

    // create device in /dev/ with the class and dev number
    struct device *my_device = device_create(my_class, NULL, dev_num, NULL, DEVICE_NAME);

    // waitqueue needs to be initialized
    init_waitqueue_head(&my_fast_device->queue);

    // cdev needs to be initialzied
    cdev_init(&my_fast_device->cdev, &fops);
    my_fast_device->cdev.owner = THIS_MODULE;
    // associate the cdev onto the dev_num, now dev_num's file contains cdev
    int errorCode = cdev_add(&my_fast_device->cdev, dev_num, 1);
    if (errorCode < 0) {
        // clean up if fail
        device_destroy(my_class, dev_num);
        class_destroy(my_class);
        unregister_chrdev_region(dev_num, 1);
        kfree(my_fast_device);
        pr_err("failed to add cdev, errorCode %d, clean up done\n", errorCode);
        return -1;
    }

    // storing the driver data so we can clean up in exit
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
        put_device(my_device); // release the reference from find_device
    }

    // destroy visible device node first
    device_destroy(my_class, dev_num);
    
    // destroy the class
    class_destroy(my_class);

    // unregister the dev num
    unregister_chrdev_region(dev_num, 1);

    // free the memory for my_fast_device and delete cdev
    if (my_fast_device) {
        cdev_del(&my_fast_device->cdev);
        kfree(my_fast_device);
    }
    pr_info("FastMod: Module unloaded\n");
}

module_init(fast_mod_init);
module_exit(fast_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("extract");
MODULE_DESCRIPTION("A kernel module");
