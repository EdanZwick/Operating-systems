/*
 * message_sender.c
 *
 *	•
Command line arguments:
1. argv[1] – message slot file path.
2. argv[2] – the write mode. Assume 0 or 1.
3. argv[3] – the target message channel id. Assume a non-negative integer.
4. argv[4] – the message to pass.
You should validate that the correct number of command line arguments is passed.
• The flow:
1. Open the specified message slot device file.
2. Set the write mode of the device.
3. Set the channel id to the id specified on the command line.
4. Write the specified message to the file.
5. Close the device.
6. Print a status message.
•
• Exit value should be 0 on success and a non-zero value on error.
Should compile without warnings or errors using gcc –O3 –Wall –std=gnu99.
 *
 *
 *  Created on: May 7, 2019
 *      Author: edan
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
	unsigned int fd, mode, channel, len;
	if (argc<5){
		puts("This function demands 4 arguments");
		return -1;
	}
	fd = open(argv[1],O_RDWR);
	if (fd<0){
		printf("Error opening file. errno: %d\n",errno);
		return -1;
	}
	mode = argv[2][0] - '0'; //Assuming 0 or 1 in ascii
	channel = atoi(argv[3]);
	len = strlen(argv[4]);
	if (ioctl(fd, MSG_SLOT_WRITE_MODE, mode)!=0 || ioctl(fd, MSG_SLOT_CHANNEL, channel)!=0){
		close(fd);
		printf("ioctel error, errno: %d\n", errno);
		return -1;
	}
	if (write(fd,argv[4],len)!=len){
		close(fd);
		printf("Write error, errno: %d\n",errno);
		return -1;
	}
	close(fd);
	printf("Successful write of %d bytes to device %s channel %d: \"%s\"\n",len,argv[1],channel,argv[4]);
	return 0;
}
