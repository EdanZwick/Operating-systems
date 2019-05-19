/*
 * message_slot.c
 *
 *  Created on: May 6, 2019
 *      Author: edan
 *
 *  Code for a kernel module - driver for pseudo hardware:
 *  This driver is used to operate multiple channel message slots.
 *  Each slot will be represented by a file on the file system.
 *
 *  Each slot has a minor number, which ranges between 0-255 as 256 is the
 * 	maximum number of devices per driver.
 *
 * 	Each slot's state is represented by three objects:
 * 	 - list of channels existing in slot.
 *	 - Current active channel.
 *	 - Write mode.
 *
 *	Each Channel's is represented by struct and it's state is given by:
 *	 - Channel number.
 *	 - 128 Byte buffer.
 *	 - Number of used bytes in buffer.
 *	 - Pointer to next channel in list (orderd by creation time, not numerical order)
 */

#undef __KERNEL__
#define __KERNEL__
#undef MODULE
#define MODULE

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "message_slot.h"

MODULE_LICENSE("GPL");

// device major number
static int major;

typedef struct c
{
	unsigned int num;
	char buffer[BUF_LEN]; //buffer containing the messages
	int index; //number of characters in current message (also the index for next messege to be written to in append*/
	struct c* next;
}channel;

static char writeMode[256]; //keeps track whether each device is in append or overwrite mode (0/1)
static channel* cur[256]; //pointer to current channel for each slot.
static channel* slots[256]; //linked list of all channels, by device (slot)

/*See documentation at top*/


//================== DEVICE FUNCTIONS ===========================
static int device_open( struct inode* inode, struct file*  file)
{
	return SUCCESS;
}

//---------------------------------------------------------------
static int device_release(struct inode* inode, struct file*  file)
{
  return SUCCESS;
}

//---------------------------------------------------------------
// a process which has already opened
// the device file attempts to read from it
static ssize_t device_read(struct file* file, char __user* buffer, size_t length, loff_t* offset)
{
	int i;
	char* cnl_buf;
	unsigned int minor = iminor(file_inode(file));
	if (cur[minor]==NULL){ //no channel selected for this slot yet
		printk(KERN_ALERT "Tried read with no channel defined for slot\n");
		return -EINVAL;
	}
	if ((cur[minor])->index == 0){ //no message exists
		printk(KERN_ALERT "Tried read from channel where no message exists\n");
		return -EWOULDBLOCK;
	}
	if (((int)(cur[minor])->index) > length){ //user buffer not large enough for entire message
		printk(KERN_ALERT "Buffer too short for read, got len %lu but message length is %d\n",length,(int)(cur[minor])->index);
		return -ENOSPC;
	}
	cnl_buf = (cur[minor])->buffer;
	for (i=0; i<(cur[minor])->index ; i++){
		if (put_user(cnl_buf[i],buffer+i)!=0){
			printk(KERN_ALERT "Read: write in user space failed\n");
			return -EFAULT;
		}
	}
	return i;
}


static ssize_t device_write( struct file* file, const char __user* buffer, size_t length, loff_t* offset)
{
	char* cnl_buf;
	int start, i;
	unsigned int minor = iminor(file_inode(file));
	if (cur[minor]==NULL){ //no channel selected for this slot yet
		printk(KERN_ALERT "Tried write with no channel defined for slot\n");;
		return -EINVAL;
	}
	start = ((cur[minor])->index)*((char)writeMode[minor]); //if append (writemode==1) we just write from end, if overwrite (writemode==0) our write will start from 0 index
	if ((start+length) > BUF_LEN || length==0){ //messege to write is 0 or too long
		printk(KERN_ALERT "messege to write is 0 or too long, got len %lu while remaining buffer is %d long\n",length,(128 - start));
		return -EMSGSIZE;
	}
	cnl_buf = ((cur[minor])->buffer)+start;
	for (i=0 ; i<length; i++){
		if (get_user(cnl_buf[i], buffer+i)!=0){
			printk(KERN_ALERT "Write: read from user space failed\n");
			return -EFAULT;
		}
	}
	(cur[minor])->index = start + i;
	return i;
}

/*
 * Auxiliary function
 *
 * Sets the working channel of a slot to the one defined by user.
 * If exists such a slot, this function finds it.
 * Else, creates a new slot with this number.
 */
int set_channel(unsigned int minor, unsigned int cnl){
	channel* prev = NULL, *tmp = slots[minor];
	if (cur[minor]!=NULL && (cur[minor])->num == cnl){ //this is already set as this slot's working channel
		return SUCCESS;
	}
	while (tmp!=NULL){
		if (tmp->num == cnl){
			cur[minor] = tmp; //this channel already exists, we will just switch this slot to it
			return SUCCESS;
		}
		prev = tmp;
		tmp = tmp->next;
	}
	tmp = kmalloc(sizeof(channel), GFP_KERNEL); //no channel with this num found. we will create a new one.
	if (tmp==NULL){
		printk(KERN_ALERT "SET CAHNNEL : Memory allocation error\n");
		return -ENOMEM;
	}
	memset(tmp,0,sizeof(channel)); //initialize channel
	tmp->num = cnl;
	if (prev==NULL){  //put this new channel in channel list for current slot
		slots[minor] = tmp; //the list was originaly empty. this is the head of the list to update in "slots" data structure
	}
	else{
		prev->next = tmp; //append new channel to end of exisiting channel list
	}
	cur[minor] = tmp; // update this new channel is the current working channel
	return SUCCESS;
}

//----------------------------------------------------------------
static long device_ioctl(struct file* file, unsigned int ioctl_command_id, unsigned long ioctl_param)
{
	unsigned int minor;
	minor = iminor(file_inode(file));
	if (ioctl_command_id == MSG_SLOT_CHANNEL && ioctl_param!=0){
		if (!set_channel(minor,ioctl_param))
			return SUCCESS;
		printk( KERN_ALERT "ioctel: couldent set new channel %lu\n",ioctl_param);
	}
	if (ioctl_command_id == MSG_SLOT_WRITE_MODE && ioctl_param<3){
		writeMode[minor] = (char) ioctl_param;
		return SUCCESS;
	}
	printk( KERN_ALERT "Ioctel failed got %u for command and %lu for param\n",ioctl_command_id,ioctl_param);
	printk( KERN_ALERT "Expected %lu for Channel and %lu for write mode\n",MSG_SLOT_CHANNEL,MSG_SLOT_WRITE_MODE);
	return -EINVAL;
}

//==================== DEVICE SETUP =============================

// This structure will hold the functions to be called
// when a process does something to the device we created
struct file_operations Fops =
{
  .read           = device_read,
  .write          = device_write,
  .open           = device_open,
  .unlocked_ioctl = device_ioctl,
  .release        = device_release,
};

//---------------------------------------------------------------
// Initialize the module - Register the character device
static int __init simple_init(void)
{
  // init data-structures
  memset(writeMode, 0, 256);
  memset(cur, 0, 256*sizeof(channel*));
  memset(slots, 0, 256*sizeof(channel*));

  // Register driver capabilities. Obtain major num
  major = register_chrdev(MAJOR_NUM, DEVICE_RANGE_NAME, &Fops);

  // Negative values signify an error
  if( major < 0 )
  {
    printk( KERN_ALERT "message_slot: registraion failed\n");
    return major;
  }
  printk(KERN_INFO "message_slot: registered major number %d\n", MAJOR_NUM);
  return SUCCESS;
}

//---------------------------------------------------------------
static void __exit simple_cleanup(void)
{
	int i;
	channel* tmp, *c;
	//free all allocated channels
	for (i=0 ; i<256 ; i++){
		if ((c = slots[i]) !=NULL){
			while (c!=NULL){
					tmp = c;
					c = c->next;
					kfree(tmp);
			}
		}
	}
	unregister_chrdev(MAJOR_NUM, DEVICE_RANGE_NAME);// Unregister the device
	printk(KERN_INFO "message_slot: unregistered major number %d\n", MAJOR_NUM);
}

//---------------------------------------------------------------
module_init(simple_init);
module_exit(simple_cleanup);

//========================= END OF FILE =========================
