/*
 * pcc_server.c
 *
 *	Server side of project.
 *	Receives a port number as a command line argument.
 *	Starts accepting connection on that port.
 *	After SIGINT is received, the number of time each printable char was received is printed.
 *
 *	Server-client protocol:
 *		Client connects and sends the number of total bytes (N) in message (as an unsigned int).
 *		Server "reads" until N bytes are recieved.
 *		Server responeds with total number of printable bytes (again as an unsigned int).
 *
 *  The data structure used for counting chars is an array where each slot holds the number times the corresponding char
 *	was received. Synchronization is kept by updating the slots using only atomic functions.
 *
 *	Overview of server's operation:
 *		Registers a handler so that Upon receiving SIGINT a flag stating not to accept new connections.
 *		Opens the specified port for listening.
 *		Delegates each new connection to a new thread.
 *		Atomically updates a counter of live threads.
 *		Detaches thread (saves the need for tracking a possible large amount of threads).
 *			Each worker thread manages a connection with one client.
 *			The worker receives the number of bytes it must receive and reads them from socket.
 *			Each thread holds it's own array where he counts the number of received chars.
 *			The worker answers the client with the total number of printable chars and closes the connection.
 *			The worker then atomically adds the number of times each char was read into the global data structure.
 *			Before exiting, each thread atomically decrements the counter of live threads.
 *			If the done flag is raised and the number of live threads is zero, this thread wakes up the main thread.
 *
 *		Main thread stops accepting new connections only after a SIGINT has been received.
 *		If there are still working threads, the main thread goes to sleep on a conditional variable.
 *		As noted above the last working thread will wake up main.
 *		(This is used as we detached our threads. We avoided responsibility for keeping track over all PID's
 *			and joining with all of them, but had to use this trick to gain back syncronization).
 *
 *  Created on: Jun 9, 2019
 *      Author: edan
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>

#define SOFFSET 32 //First printable char
#define EOFFSET 126 //Last printable char
#define TOTAL (EOFFSET - SOFFSET + 1) //Total number of printable
#define CONNECTION_QUEUE_SIZE 100 //Number of connections we are alowed to hold in queue for accept
#define BUF_LEN 4096 //number of bytes to be read from connection at each time.
#define NET_CHECK(invoker, err_msg) { \
  if (invoker == -1) { \
    if (listenfd!=-1) \
		close(listenfd); \
	perror(err_msg); \
	return 1; \
  } \
}//macro to reduce redundant lines in main
#define CHECK_SERVE(invoker, err_msg) { \
  if (invoker<=0) { \
    if (buffer!=NULL)\
		free(buffer);\
	close(fd); \
	perror(err_msg); \
	quit(); \
	pthread_exit((void*)1); \
  } \
}\


int register_sig();
void sig_handler(int signum, siginfo_t *info, void *ptr);
void* serve(void* connfd);
void quit();
int init();
int get_lis_port(int *fd, int port);


unsigned long pcc_count[EOFFSET - SOFFSET + 1] = {0}; //counter array for each of printable chars
char done = 0;
int running = 0; //counter to keep track of current running threads
pthread_mutex_t f_mutex; // Auxiliary lock used for conditional variable
pthread_cond_t  finished; // wait for all threads to finish working

int main(int argc, char *argv[]){
	//Parse arguments
	if (argc<2){
		fprintf(stderr,"Usage <Port num>\n");
		return 1;
	}
	//register custor signal handler for sigint (ctrl c)
	if (register_sig() != 0){
		return 1;
	}
	//initialize data structure to count chars, and required conditional variables and locks.
	if (init()!=0){
		return 1;
	}

	//create a listening socket for main to use.
	unsigned int port = atoi(argv[1]);
	int listenfd;
	if (get_lis_port(&listenfd,port)!=0){
		return 1;
	}

	//start accepting connections, subtask each connection to a new thread
    long connfd = -1; //will hold new socket fd deliverd to each new serving thread
    pthread_t threadID; //will hold tid for each new created thread (before it will be detached and we can discard this)

    while (!done){
		connfd = accept(listenfd, NULL, NULL);
		if(connfd < 0){
			if (done){
				break;
			}
			perror("Accept Failed. :(");
			close(listenfd);
			return 1;
	    }
	    __sync_fetch_and_add(&running, 1); //update another thread is being created
	    if (pthread_create(&threadID, NULL, serve, (void*)connfd)!=0){
	    	printf("Error: Failed to create thread.\n");
	    	close(connfd);
	    	close(listenfd);
	    	//pthread_attr_destroy(&attr);
	    	return 1;
	    }
	    if (pthread_detach(threadID)!=0){
	    	printf("Error: Failed to detach thread.\n");
			close(connfd);
			close(listenfd);
			return 1;
	    }
	}
    // This part of code can only be reached after sigint was recieved
    // As this might happen after a very long run time, there is not need to keep all the thread resources available.
    // Thus all threads are detached. But we still need a way to "wait" on working threads before quiting.
    // This is done using a counter for active threads. This main thread will sleep until the last working thread quit's.
    close(listenfd); //finished with this socket
    if (pthread_mutex_lock(&f_mutex)){ //failed acquiring mutex
		fprintf(stderr,"error in mutex acquire");
		return 1;
	}
    while (running!=0){
    	pthread_cond_wait(&finished, &f_mutex);
    }
    if (pthread_mutex_unlock(&f_mutex)){
		fprintf(stderr,"error in mutex release\n");
		return 1;
	}
    //print loop
    for (int i=0; i < TOTAL ; i++){
    	printf("char '%c' : %lu times\n",i+SOFFSET,pcc_count[i]);
    }
	pthread_mutex_destroy(&f_mutex);
	pthread_cond_destroy(&finished);
	return 0; //Again, can only be reached if no other threads are alive
}

/*
 * Serves connection with one client.
 * Receives as an argument the socket ID for connection it is responsible for.
 *
 * Connection protocol with user - Sizeof(int) = 4 first bytes are "N": the number of bytes the user intends to send.
 * When N bytes have been received: this thread replies the number of printable chars and exits.
 */
void* serve(void* connfd){
    char* buffer = NULL;
	long fd = (int)(long)connfd;
	unsigned int len = 0; //holds the message length
	unsigned int count[TOTAL] = {0}; //counter array for each of printable chars, defined here as assignment required
	//only one update. hopefully this will be cleared up
	char c = 0; //will be used when going over the buffer
	int batch, i;
	unsigned int printable = 0; //stores counter for printable

	//Get expected msg length
	int r = 0; //first 4 bytes are message length
	int tmp;
	while (r < sizeof(unsigned int)){ //read first 4 bytes
		tmp = read(fd,&len+r,sizeof(unsigned int)-r);
		CHECK_SERVE(tmp,"Error reading length from socket")
		r += tmp;
	}

	// Start reading msg.
	// Naive way is to read byte-by-byte, though this might be very inefficient. On the other side, msg len is bound only
	// by max(unsigned int) = 2^32 - 1 roughly 4 GB. allocating all this also won't work.
	// instead we will read from the socket 4KB at a time (defined in macro BUF_LEN).
	buffer = malloc(BUF_LEN * sizeof(char));
	if (buffer==NULL){
		fprintf(stderr,"Allocating buffer for read failed\n");
		close(fd);
		quit();
		pthread_exit((void*)1);
	}
	while (len>0){ //as long as we have more batches to read
		batch = (len < BUF_LEN) ? len : BUF_LEN; //read 4096 bytes or just the remainder if smaller than 4096
		len -= batch; //this is the number left to read after this batch
		r = 0;
		while (r < batch){ //read batch
			tmp = read(fd,buffer+r,batch-r);
			CHECK_SERVE(tmp,"Error reading bytes from socket")
			r += tmp;
		}
		for (i=0; i < batch ; i++){ //count chars in batch
			c = buffer[i];
			if (c >= SOFFSET && c <= EOFFSET){
				printable++;
				count[c-SOFFSET]++; //increment counter for char
			}
		}
	}
	free(buffer);

	// Answer with the number of printable bytes
	r = 0;
	while (r<sizeof(unsigned int)){
		tmp += write(fd,&printable,sizeof(unsigned int)-r);
		CHECK_SERVE(tmp,"Error while writing back length")
		r += tmp;
	}
	close(fd);

	//finished communication with client, now update the global data-structure
	for (int i = TOTAL-1; i>=0 ;i--){
		if (count[i]==0){ //nothing to update here
			continue;
		}
		__sync_fetch_and_add((unsigned long *)(pcc_count + i),count[i]);
	}
	quit(); //clean resorces and wake up main if SIGINT was received and this is the last working thread
	pthread_exit(0);
}


/*
 * Decrements working thread counter, and wakes sleeping main thread if sigint was recevied
 * and this is the last working thread.
 *
 */
void quit(){
	__sync_fetch_and_sub(&running,1);
	if (running==0 && done){
		pthread_mutex_lock(&f_mutex);
		pthread_cond_signal(&finished); //signal main all sub threads finished working
		pthread_mutex_unlock(&f_mutex);
	}
}


/*
 * Initializes all resources needed for the program.
 * Separated from main to improve readability.
 *
 */
int init(){
	// All my threads are going to work in detached mode. This way I won't need to keep track of thier ID's
	// Which might be a large ammount if the program works for a long time.
	// So I will initialize threads as detached (now this will require me to wait in a different way on threads instead of join)
	if (pthread_mutex_init(&f_mutex, NULL)){
		fprintf(stderr,"Failure initializing f lock\n");
		return 1;
	}
	if (pthread_cond_init (&finished, NULL)){
		pthread_mutex_destroy(&f_mutex);
		fprintf(stderr,"Failure conditional variable\n");
		return 1;
	}

	return 0;
}

/*
 * Creates a new listening socket for main thread.
 * On success updates the new socket fd in variable pointed by fd and returns 0.
 * On failure returns 1
 *
 * Separated from main to improve readability
 *
 * This function utilizes the NET_CHECK macro to print error, free open fd's and return,
 * in case socket, bind or listen fail.
 *
 */
int get_lis_port(int *fd, int port){
	//Get fd for socket
	int listenfd = -1;
	int tmp = 0;
	listenfd = socket(AF_INET, SOCK_STREAM, 0);
    NET_CHECK(listenfd,"Failed creating listening socket"); //error handling macro for network API components

    //configure the socket to bind to all interfaces, in specified port
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    tmp = bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr_in));
    NET_CHECK(tmp,"Failed binding socket\n");

    tmp = listen(listenfd, CONNECTION_QUEUE_SIZE);
    NET_CHECK(tmp, "Failed to start listening to incoming connections\n");

    //All done
    *fd = listenfd;
    return 0;
}

/*
 *	Assigns new signal handler for SIGINT.
 *	Separated from main in order to improve readability.
 */
int register_sig(){
	//Assign new handler for sigint
	struct sigaction new_action;
	memset(&new_action, 0, sizeof(new_action));
	new_action.sa_sigaction = sig_handler;
	new_action.sa_flags = SA_SIGINFO;
	if (sigaction(SIGINT, &new_action, NULL)){
		perror("Failure assigning sig handler\n");
		return 1;
	}
	return 0;
}

/*
 * Custom sig handler for SIGINT
 * Raises flag indicating to stop accepting new connections (used by main).
 */
void sig_handler(int signum, siginfo_t *info, void *ptr){
	printf("\n\n\n\n SIGINT caught %ld. Wrapping it up.\n \n\n\n\n",pthread_self());
	__sync_fetch_and_add(&done, 1);
	return;
}


