#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/slab.h>

#define DRIVER_NAME "hc06_bt_serdev"
#define DEVICE_NAME "hc06_bt"

static int major_number;
static struct class *my_class;
static struct device *my_dev;
struct serdev_device *my_serdev;
struct mutex my_lock;
char rx_buffer[256];
size_t rx_len;
wait_queue_head_t read_queue;

static ssize_t my_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    if (wait_event_interruptible(read_queue, rx_len > 0))
        return -ERESTARTSYS;

    mutex_lock(&my_lock);
    if (count > rx_len)
        count = rx_len;

    if (copy_to_user(buf, rx_buffer, count)) {
        mutex_unlock(&my_lock);
        return -EFAULT;
    }

    rx_len = 0;
    mutex_unlock(&my_lock);

    return count;
}

static ssize_t my_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char *kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    int ret = serdev_device_write_buf(my_serdev, kbuf, count);
    kfree(kbuf);

    return ret < 0 ? ret : count;
}

static const struct file_operations hc06_bt_fops = {
    .owner = THIS_MODULE,
    .read = my_read,
    .write = my_write
};

static int my_receive_buf(struct serdev_device *serdev, const unsigned char *buf, size_t size) {
    mutex_lock(&my_lock);
    if (size > sizeof(rx_buffer) - rx_len)
        size = sizeof(rx_buffer) - rx_len;

    memcpy(rx_buffer + rx_len, buf, size);
    rx_len += size;
    mutex_unlock(&my_lock);
    wake_up_interruptible(&read_queue);

    return size;
}

static const struct serdev_device_ops hc06_bt_serdev_ops = {
    .receive_buf = my_receive_buf,
    .write_wakeup = NULL,
};

static int my_probe(struct serdev_device *serdev) {
    pr_info("%s: Probing\n", DRIVER_NAME);

    mutex_init(&my_lock);
    init_waitqueue_head(&read_queue);
    my_serdev = serdev;

    serdev_device_set_client_ops(serdev, &hc06_bt_serdev_ops);
    int ret = serdev_device_open(serdev);
    if (ret) {
        dev_err(&serdev->dev, "Failed to open serdev device: %d\n", ret);
        return ret;
    }

    serdev_device_set_baudrate(serdev, 9600);
    serdev_device_set_flow_control(serdev, false);
    serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);

    major_number = register_chrdev(0, DEVICE_NAME, &hc06_bt_fops);
    if (major_number < 0) {
        serdev_device_close(serdev);
        return major_number;
    }

    my_class = class_create(THIS_MODULE, "bluetooth");
    if (IS_ERR(my_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        serdev_device_close(my_serdev);
        return PTR_ERR(my_class);
    }

    my_dev = device_create(my_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(my_dev)) {
        class_destroy(my_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        serdev_device_close(my_serdev);
        return PTR_ERR(my_dev);
    }

    pr_info("%s: Probed successfully\n", DRIVER_NAME);
    return 0;
}

static void my_remove(struct serdev_device *serdev) {
    device_destroy(my_class, MKDEV(major_number, 0));
    class_unregister(my_class);
    class_destroy(my_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    serdev_device_close(serdev);
    pr_info("%s: Removed\n", DRIVER_NAME);
}

static const struct of_device_id hc06_bt_of_match[] = {
    { .compatible = "hc-06,mocne", },
    { },
};
MODULE_DEVICE_TABLE(of, hc06_bt_of_match);

static struct serdev_device_driver hc06_bt_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = hc06_bt_of_match,
    },
    .probe = my_probe,
    .remove = my_remove,
};

module_serdev_device_driver(hc06_bt_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lampaBiutkowa");
MODULE_DESCRIPTION("HC-06 Bluetooth Driver");