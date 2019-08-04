/*
 * pcc_client.c
 *
 *	Recieves host (name or ip), port number and legnth as command line arguments.
 *	Opens a connection with host and sends *length* random bytes to host.
 *	Waits for a respond from server containing the number of printable (ascii range: ' '-'~') bytes sent.
 *
 *	As length isn't bounded, we read and send data to server as batches of 4096 Bytes.
 *
 *  Created on: Jun 9, 2019
 *      Author: edan
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#define OUT_PATH "/dev/urandom"
#define BUF_LEN 4096
#define CHECK(invoker, err_msg){ \
  if (invoker){ \
    perror(err_msg); \
	if (rand!=-1) \
		close(rand); \
	if (fd!=-1){ \
		close(fd); \
		puts("closed fd");\
	}\
	if (buffer!=NULL) \
		free(buffer); \
	return 1; \
  } \
}\

int get_connection(char *name, char* port, int* fd);

int main(int argc, char *argv[]){
	int batch;
	int fd = -1;
	int rand = -1;
	//Parse arguments
	if (argc<4){
		fprintf(stderr,"Usage <Host> <Port> <msg length>\n");
		return 1;
	}

	//get wanted msg len from argument
	unsigned int len = atoi(argv[3]);
	long actual_len = len +  sizeof(unsigned int); //including header, long as theoretically adding 4 could cause an overflow

	//As in server, becuase msg length might be huge, It will be read and sent 4KB at a time.
	char* buffer = (char*) malloc(BUF_LEN*sizeof(char)); //allocate space for payload (length + msg)
	if (buffer==NULL){
		printf("error allocating buffer\n");
		return 1;
	}
	memcpy(buffer, &len, sizeof(unsigned int)); //copy the message length as the first 4 bytes

	//Open file to read random bytes
	rand = open(OUT_PATH,O_RDONLY);
	CHECK((rand<0),"error opening file rand")

	//Find address to bind for this host
	CHECK((get_connection(argv[1],argv[2],&fd)!=0),"Couldn't get address for connection\n")

	//Start reading and sending 4KB at a time
	int r = sizeof(unsigned int); //offset as we already put length into the first batch
	int tmp;
	while (actual_len>0){
		batch = (actual_len < BUF_LEN) ? actual_len : BUF_LEN; //read 4096 bytes or just the remainder if smaller than 4096
		// fill buffer with batch
		while (r<batch){
			tmp = read(rand,buffer+r,batch-r);
			CHECK((tmp<=0),"failed at reading rand")
			r+=tmp;
		}
		r = 0;
		// Send current buffer
		while (r < batch){
			tmp = write(fd,buffer+r,batch-r);
			CHECK((tmp<=0),"failed at sending batch (write)\n")
			r += tmp;
		}
		actual_len -= batch; //this is the number left to read after this batch
		r = 0;
	}
	close(rand);
	rand = -1; //won't close this again
	free(buffer);
	buffer = NULL; //won't free this again

	//finished sending, now wait for response
	r = 0;
	unsigned int ans;
	while (r < sizeof(unsigned int)){
		tmp = read(fd,&ans+r,sizeof(unsigned int)-r);
		CHECK((tmp<0),"Error receiving answer (read)")
		r += tmp;
	}
	close(fd);
	printf("# of printable characters: %u\n", ans);
	return 0;
}

/*
 * Resolves connection address for name:port.
 * On success updates open socket fd on fd and returns 0.
 * On failure returns 1
 */
int get_connection(char *name, char* port, int* fd){
	struct addrinfo hints, *res, *rp;
	int sfd;
    //init the addrinfo struct for name configuration
	memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;    /* Allow IPv4*/
    hints.ai_socktype = SOCK_STREAM; /* TCP */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          /* Any protocol */
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    //send the query
    if (getaddrinfo(name, port, &hints, &res)){
    	fprintf(stderr,"Failed to get addr info\n");
    	return 1;
    }

    //query might return several options, we try them in order until we get a connection
    for (rp = res; rp != NULL; rp = rp->ai_next) {
	   sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
	   //handle socket errors
	   if (sfd == -1){
		   continue;
	   }
	   if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1){
		   break; //Success
	   }
	   close(sfd);
	}
    freeaddrinfo(res);
    if (rp==NULL){
		fprintf(stderr,"Failed to resolve host name (get addr info)\n");
		return 1;
    }
    *fd = sfd;
    return 0;
}
