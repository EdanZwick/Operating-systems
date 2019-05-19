/*
 * message_slot.h
 *
 *  Created on: May 7, 2019
 *      Author: edan
 */

#ifndef MESSAGE_SLOT_H_
#define MESSAGE_SLOT_H_


#define MAJOR_NUM 243
#define MSG_SLOT_CHANNEL _IOW(MAJOR_NUM, 1, unsigned long)
#define MSG_SLOT_WRITE_MODE _IOW(MAJOR_NUM, 0, unsigned long)
#define BUF_LEN 128
#define SUCCESS 0
#define FAILURE -1
#define DEVICE_RANGE_NAME "message_slot_driver"


#endif /* MESSAGE_SLOT_H_ */
