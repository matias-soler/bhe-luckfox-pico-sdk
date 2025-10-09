#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/serdev.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/ioport.h>
#include <linux/mod_devicetable.h>

static struct serdev_device *serdev_device;

#define PACKET_SIGNATURE 0xBADF00D
#define BUF_SIZE 2048
#define RX_BUFFER_SIZE 2048
#define HEADER_SIZE 8

static char uart_buffer[BUF_SIZE];
static size_t buffer_head = 0;
static size_t buffer_tail = 0;
static wait_queue_head_t uart_wait_queue;

static char rx_buffer[RX_BUFFER_SIZE];
static size_t rx_buffer_pos = 0;
static DEFINE_MUTEX(buffer_lock);

static int baud_rate = 9600; // Adjust to match enclave hardware (e.g., 115200 if needed)

static unsigned char root_state = 0;
static unsigned char version = 0;

static struct class *enclave_class;

static ssize_t root_state_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return scnprintf(buf, PAGE_SIZE, "%u\n", root_state);
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return scnprintf(buf, PAGE_SIZE, "%u\n", version);
}

static DEVICE_ATTR_RO(root_state);
static DEVICE_ATTR_RO(version);

static struct attribute *enclave_attrs[] = {
    &dev_attr_root_state.attr,
    &dev_attr_version.attr,
    NULL,
};

static const struct attribute_group enclave_group = {
    .attrs = enclave_attrs,
};

static void uart_write_wakeup(struct serdev_device *serdev) {
    printk(KERN_INFO "UART write wakeup for %s\n", dev_name(&serdev->dev));
}

static int uart_receive_buf(struct serdev_device *serdev, const unsigned char *data, size_t count) {
    size_t i;
    unsigned short packet_type;
    unsigned int signature;
    unsigned int payload_size;
    unsigned char *payload;

    if (!data || count == 0) {
        printk(KERN_ERR "Invalid receive_buf parameters\n");
        return 0;
    }

    if (rx_buffer_pos + count > RX_BUFFER_SIZE) {
        printk(KERN_ERR "Receive buffer overflow\n");
        rx_buffer_pos = 0;
        return 0;
    }

    memcpy(&rx_buffer[rx_buffer_pos], data, count);
    rx_buffer_pos += count;

    while (rx_buffer_pos >= HEADER_SIZE) {
        signature = le32_to_cpu(*(uint32_t *)rx_buffer);
        if (signature != PACKET_SIGNATURE) {
            printk(KERN_WARNING "Invalid packet signature, discarding byte\n");
            memmove(rx_buffer, &rx_buffer[1], --rx_buffer_pos);
            continue;
        }

        packet_type = le16_to_cpu(*(uint16_t *)(rx_buffer + 4));
        payload_size = le16_to_cpu(*(uint16_t *)(rx_buffer + 6));

        if (rx_buffer_pos < HEADER_SIZE + payload_size) {
            break;
        }

        payload = &rx_buffer[HEADER_SIZE];

        if (packet_type == 0x0004) {
            if (payload_size >= 2) {
                root_state = payload[0];
                version = payload[1];
                printk(KERN_INFO "State Update Packet Received\n");
                printk(KERN_INFO "Root State: %u\n", root_state);
                printk(KERN_INFO "Version: %u\n", version);
            } else {
                printk(KERN_ERR "State Update packet payload too small\n");
            }
        } else {
            printk(KERN_INFO "Unhandled packet type: 0x%04X\n", packet_type);
            mutex_lock(&buffer_lock);
            for (i = 0; i < HEADER_SIZE + payload_size; i++) {
                uart_buffer[buffer_head] = rx_buffer[i];
                buffer_head = (buffer_head + 1) % BUF_SIZE;
                if (buffer_head == buffer_tail) {
                    printk(KERN_WARNING "UART buffer overflow, dropping data\n");
                    buffer_tail = (buffer_tail + 1) % BUF_SIZE;
                }
            }
            wake_up_interruptible(&uart_wait_queue);
            mutex_unlock(&buffer_lock);
        }

        memmove(rx_buffer, &rx_buffer[HEADER_SIZE + payload_size], rx_buffer_pos - (HEADER_SIZE + payload_size));
        rx_buffer_pos -= (HEADER_SIZE + payload_size);
    }

    return count;
}

static ssize_t uart_misc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    size_t bytes_read = 0;

    if (buffer_head == buffer_tail) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        wait_event_interruptible(uart_wait_queue, buffer_head != buffer_tail);
    }

    mutex_lock(&buffer_lock);
    while (buffer_head != buffer_tail && bytes_read < count) {
        if (put_user(uart_buffer[buffer_tail], &buf[bytes_read])) {
            mutex_unlock(&buffer_lock);
            return -EFAULT;
        }
        buffer_tail = (buffer_tail + 1) % BUF_SIZE;
        bytes_read++;
    }
    mutex_unlock(&buffer_lock);

    return bytes_read;
}

static ssize_t uart_misc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    int ret;
    char kbuf_stack[128];
    char *kbuf = kbuf_stack;
    bool needs_free = false;

    if (count == 0)
        return 0;

    if (count > sizeof(kbuf_stack)) {
        kbuf = kmalloc(count, GFP_KERNEL);
        if (!kbuf)
            return -ENOMEM;
        needs_free = true;
    }
    
    if (copy_from_user(kbuf, buf, count)) {
        if (needs_free)
            kfree(kbuf);
        return -EFAULT;
    }
    
    ret = serdev_device_write_buf(serdev_device, kbuf, count);
    if (needs_free)
        kfree(kbuf);

    return ret < 0 ? ret : count;
}

static const struct file_operations uart_fops = {
    .owner = THIS_MODULE,
    .read = uart_misc_read,
    .write = uart_misc_write,
    .llseek = no_llseek,
};

static struct miscdevice uart_misc_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "enclave",
    .fops = &uart_fops,
    .mode = 0666,
};

static const struct serdev_device_ops uart_ops = {
    .receive_buf = uart_receive_buf,
    .write_wakeup = uart_write_wakeup,
};

static int my_module_probe(struct serdev_device *serdev) {
    struct serdev_controller *ctrl;
    struct device *sysfs_dev;
    struct device *ctrl_dev;
    struct platform_device *pdev;
    struct resource *res;
    int ret;

    printk(KERN_INFO "UART Probe Called for %s\n", dev_name(&serdev->dev));
    serdev_device = serdev;
    ctrl = serdev->ctrl;

    if (!ctrl) {
        printk(KERN_ERR "serdev->ctrl is NULL\n");
        return -ENODEV;
    }

    ctrl_dev = &ctrl->dev;
    pdev = to_platform_device(ctrl_dev);
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res || res->start != 0xff4d0000UL) {
        printk(KERN_INFO "Skipping non-UART3 device: %s\n", dev_name(&serdev->dev));
        return -ENODEV;
    }

    if (!ctrl->ops) {
        printk(KERN_ERR "serdev->ctrl->ops is NULL\n");
        return -ENODEV;
    }

    if (!ctrl->ops->set_baudrate) {
        printk(KERN_ERR "set_baudrate is not implemented by the controller\n");
        return -ENOTSUPP;
    }

    enclave_class = class_create(THIS_MODULE, "enclave");
    if (IS_ERR(enclave_class)) {
        printk(KERN_ERR "Failed to create enclave class: %ld\n", PTR_ERR(enclave_class));
        return PTR_ERR(enclave_class);
    }

    sysfs_dev = device_create(enclave_class, &serdev->dev, 0, NULL, "enclave-%s", dev_name(&serdev->dev));
    if (IS_ERR(sysfs_dev)) {
        printk(KERN_ERR "Failed to create sysfs device: %ld\n", PTR_ERR(sysfs_dev));
        class_destroy(enclave_class);
        return PTR_ERR(sysfs_dev);
    }
    dev_set_drvdata(&serdev->dev, sysfs_dev);

    ret = sysfs_create_group(&sysfs_dev->kobj, &enclave_group);
    if (ret) {
        printk(KERN_ERR "Failed to create sysfs group: %d\n", ret);
        device_destroy(enclave_class, 0);
        class_destroy(enclave_class);
        return ret;
    }

    serdev_device_set_client_ops(serdev, &uart_ops);

    ret = serdev_device_open(serdev);
    if (ret) {
        printk(KERN_ERR "Failed to open serdev device: %d\n", ret);
        sysfs_remove_group(&sysfs_dev->kobj, &enclave_group);
        device_destroy(enclave_class, 0);
        class_destroy(enclave_class);
        return ret;
    }

    serdev_device_set_baudrate(serdev, baud_rate);
    init_waitqueue_head(&uart_wait_queue);

    ret = misc_register(&uart_misc_device);
    if (ret) {
        printk(KERN_ERR "Failed to register misc device: %d\n", ret);
        serdev_device_close(serdev);
        sysfs_remove_group(&sysfs_dev->kobj, &enclave_group);
        device_destroy(enclave_class, 0);
        class_destroy(enclave_class);
        return ret;
    }

    printk(KERN_INFO "UART device probed successfully\n");
    return 0;
}

static void uart_serdev_remove(struct serdev_device *serdev) {
    struct device *sysfs_dev = dev_get_drvdata(&serdev->dev);

    printk(KERN_INFO "Removing UART device\n");
    misc_deregister(&uart_misc_device);
    serdev_device_close(serdev);
    sysfs_remove_group(&sysfs_dev->kobj, &enclave_group);
    device_destroy(enclave_class, sysfs_dev->devt);
    class_destroy(enclave_class);
}

static const struct of_device_id my_module_of_match[] = {
    { .compatible = "rockchip,rv1106-uart" },
    { }
};

MODULE_DEVICE_TABLE(of, my_module_of_match);

static struct serdev_device_driver my_module_driver = {
    .driver = {
        .name = "card-enclave-driver",
        .of_match_table = my_module_of_match,
        .owner = THIS_MODULE,
    },
    .probe = my_module_probe,
    .remove = uart_serdev_remove,
};

static int __init enclave_init(void) {
    int ret;

    ret = driver_register(&my_module_driver.driver);
    if (ret) {
        printk(KERN_ERR "Failed to register serdev driver: %d\n", ret);
        return ret;
    }

    return 0;
}

static void __exit enclave_exit(void) {
    driver_unregister(&my_module_driver.driver);
}

module_init(enclave_init);
module_exit(enclave_exit);

module_param(baud_rate, int, 0644);
MODULE_PARM_DESC(baud_rate, "UART baud rate (default 9600)");
MODULE_DESCRIPTION("Hello World");
MODULE_AUTHOR("Matias Sebastian Soler <matias.s.soler@gmail.com>");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");