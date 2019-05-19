/*
 * message_reader.c
 *
 *  Created on: May 8, 2019
 *      Author: edan
 *
 *      Command line argument:
1. argv[1] – message slot file path.
2. argv[2] – the target message channel id. Assume a non-negative integer.
You should validate that the correct number of command line arguments is passed.
The flow:
1. Open the specified message slot device file.
2. Set the channel id to the id specified on the command line.
3. Read a message from the device to a buffer.
4. Close the device.
5. Print the message and a status message.
Exit value should be 0 on success and a non-zero value on error.
Should compile without warnings or errors using gcc –O3 –Wall –std=gnu99.
 */


#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "message_slot.h"


int main (int argc, char* argv[]){
	unsigned int fd, channel;
	int len;
	char buf[BUF_LEN];
	memset(buf,0,BUF_LEN);
	if (argc<3){
		printf("This function demands 2 arguments");
		return -1;
	}
	fd = open(argv[1],O_RDWR);
	if (fd<0){
		printf("Error opening file, errno: %d\n",errno);
		return -1;
	}
	channel = atoi(argv[2]);
	if (ioctl(fd, MSG_SLOT_CHANNEL, channel)!=0){
		close(fd);
		printf("ioctel error - reader, to file %s with channel %d, errno: %d\n",argv[1],channel,errno);
		return -1;
	}
	if ((len = read(fd,&buf,BUF_LEN)) < 0){
		close(fd);
		printf("Read error. errno : %d\n",errno);
		return -1;
	}
	close(fd);
	printf("Successful read of %d bytes from device %s channel %d\n",len,argv[1],channel);
	printf("got message \"%.*s\"\n",len,buf);
	return 0;
}

