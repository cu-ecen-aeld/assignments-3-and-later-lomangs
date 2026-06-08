/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("lomangs"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

static int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

static int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

static ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset_byte = 0;
    size_t bytes_to_copy = 0;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    // 1. Acquire mutex safely
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // 2. Find the entry corresponding to the current global file position (*f_pos)
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset_byte);

    // If no entry exists at this offset, we have reached the End-Of-File (EOF)
    if (!entry) {
        retval = 0; 
        goto out;
    }

    // 3. Determine how many bytes we can actually return to the user
    // Calculated as the remaining bytes in this entry up to the requested 'count'
    bytes_to_copy = entry->size - entry_offset_byte;
    if (bytes_to_copy > count) {
        bytes_to_copy = count;
    }

    if (copy_to_user(buf, entry->buffptr + entry_offset_byte, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    // 5. Advance file position and set our return value to the number of read bytes
    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

    out:
       mutex_unlock(&dev->lock);
       return retval;
}

static ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *new_alloc = NULL;
    const char *freed_entry_buff = NULL;
    struct aesd_buffer_entry new_entry;
    size_t i;
    bool newline_found = false;
    
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
    // 1. Thread safety: Acquire the device mutex lock
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    // 2. Allocate or expand memory to accommodate the incoming chunk of data
    // krealloc acts like kmalloc if dev->tmp_buffer is NULL
    new_alloc = krealloc(dev->tmp_buffer, dev->tmp_buffer_size + count, GFP_KERNEL);
    if (!new_alloc) {
        retval = -ENOMEM;
        goto out;
    }
    dev->tmp_buffer = new_alloc;

    // 3. Copy the data block from user space into our updated kernel buffer
    if (copy_from_user(dev->tmp_buffer + dev->tmp_buffer_size, buf, count)) {
        retval = -EFAULT;
        goto out;
    }

    // 4. Update the buffer tracker size with the actual copied bytes
    dev->tmp_buffer_size += count;
    retval = count;                                                                                                                                                     
    // 5. Scan the newly appended data bytes to look for a command terminator (\n)
    // Check if a newline exists anywhere in the newly written block
    for (i = dev->tmp_buffer_size - count; i < dev->tmp_buffer_size; i++) {
       if (dev->tmp_buffer[i] == '\n') {
           newline_found = true;
           break;
       }
    }

    // 6. If a newline is found, finalize this entry
    if (newline_found) {
        new_entry.buffptr = dev->tmp_buffer;
        new_entry.size = dev->tmp_buffer_size;

        // Add the entry to the circular buffer
        freed_entry_buff = aesd_circular_buffer_add_entry(&dev->buffer, &new_entry);
        if (freed_entry_buff != NULL) {
            kfree(freed_entry_buff);
        }

        // Reset tracking fields so the next write starts fresh
        dev->tmp_buffer = NULL;
        dev->tmp_buffer_size = 0;
    }

   out:
       mutex_unlock(&dev->lock);
       return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



static int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    // 1. Initialize the thread-safety mutex lock
    mutex_init(&aesd_device.lock);

    // 2. Initialize your Assignment 7 circular buffer structures
    aesd_circular_buffer_init(&aesd_device.buffer);

    // 3. Ensure the temporary write buffer tracking is zeroed/nullified
    // Note: This is already covered by the memset above, but it serves as 
    // good documentation for what variables are tracking state.
    aesd_device.tmp_buffer = NULL;
    aesd_device.tmp_buffer_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

static void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    // 2. Free any partial, unterminated write command allocations left behind
    if (aesd_device.tmp_buffer != NULL) {
        kfree(aesd_device.tmp_buffer);
        aesd_device.tmp_buffer = NULL;
    }

    // 3. Free all valid buffer history entries currently stored in the circular buffer
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr != NULL) {
            kfree(entry->buffptr);
            // Optional: set to NULL to prevent dangling references
            entry->buffptr = NULL; 
        }
    }

    // 4. Destroy the initialized synchronization mutex lock
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
