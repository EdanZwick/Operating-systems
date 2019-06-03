/*
 * distributed_subdir_size.c
 *
 *  Created on: May 27, 2019
 *      Author: edan
 *
 * C code to exercise using threading.
 * This program receives two command line arguments:
 * 		Dir - Name of a directory - used as a root for traversing directories.
 * 		N - Number of threads.
 *
 * For each directory in Dir's subtree, the programs sums the file sizes that lay *directly* inside a directory
 * (meaning: files contained in a sub-directory are not counted in their forefathers size).
 *
 * Search starts from directory "dir", and work is distributed by N threads.
 *
 * Search is organized using a linked queue: each thread dequeues a directory name and starts to sum it's files's sizes.
 * As multiple threads are working simultaneously, concurrency issues are dealt with using conditional variables and
 * mutex locks (only one thread can edit queue at each point).
 *
 * To avoid resource waste, if queue is empty threads waiting to dequeue wait on conditional variable "empty" until
 * a new item is queued.
 *
 * The program uses two counters for number of live threads and number of idle threads.
 * When a thread encounters an empty queue and all other threads are idle, this means the work is done.
 * The last remaining threads falgs search is over and wakes all waiting threads.
 *
 *
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <libgen.h>

#define EMPTY ((q.head)==NULL) //macro to check state of queue
#define ALL_IDLE (idle >= (total-1)) //macro to check if all threads are idle
#define LAST (idle == (total-1)) //macro to check if all threads but this one are idle
#define CHECK(invoker, err_msg) { \
  if (invoker) { \
    fprintf(stderr, err_msg); \
	exit(1); \
  } \
}//macro to reduce redundant lines in main
#define CHECK_THREAD(invoker, err_msg) { \
  if (invoker) { \
	if (name!=NULL)\
		free(name); \
	alive[serial]=0; \
	__sync_fetch_and_sub(&total, 1);\
	pthread_cond_signal(&empty);\
	fprintf err_msg; \
	pthread_exit((void*)1); \
  } \
} //macro to reduce redundant lines in thread_do.
//condvar is signaled, in case this dying thread was the last one which all were waiting for.

typedef struct n{
	char* name;
	struct n* next;
}node;

typedef struct q{
	node* head;
	node* tail;
}queue;

typedef struct d{
	char* name;
	unsigned long size;
} dir;


int init(int);
void destroy();
int register_sig();
char* dequeue(int* done);
int enque(char* , char*);
void* thread_do();
node* make_node(char* dir, char* buffer);
void destroy_node(node* n);
void destroy_q();
void sig_handler(int signum, siginfo_t *info, void *ptr);
void change_max(char* name, int size, long serial);
int get_size(char* name, long serial);
void clean_lock(void* lock);

int num = 0; //number of total user requested threads
int idle = 0; //will count the number of idle threads
int total = 0; //will count the total number of active threads (created and did not die)
int finished = 0; //flag whether to keep waiting for new items in queue
pthread_t* threads; //will hold array of threads
char* alive = NULL; //alive[i] will keep a boolean attribute whether thread i is still alive
queue q;
dir max; //details of largest directory so far.
pthread_mutex_t q_mutex; //access control for queue
pthread_cond_t  empty; //wait if queue is empty
pthread_mutex_t max_mutex; //access control for max struct

int main(int argc, char* argv[]){
	int tmp;
	CHECK(register_sig(),"exiting..") //register signal handler for SIGINT (separated to reduce code bloat)
	//Check valid input and initialize structures
	if (argc<3){
		fprintf(stderr,"Usage <directory> <Number of threads>, not enough variables\n");
		exit(1);
	}
	num = atoi(argv[2]); //number of wanted threads
	CHECK(init(num),"exiting.."); // initialize all locks, cond vars and thread array
	CHECK(enque("",argv[1]+1),"exiting.."); //add home directory to queue, +1 as '/' will be added automatically by enqueue
	//Start creating threads
	long i=0;
	while (i < num && !finished){ //create threads
		__sync_fetch_and_add(&total, 1);// update one more created thread (will be used to tell if all threads are idle)
		alive[i] = 1; //no need for atomicity, as only this thread will edit this slot
		CHECK(pthread_create(threads+i,NULL,thread_do,(void*)i),"error in creating thread\n");
		//printf("created thread %ld\n",i);
		i++;
	}
	//Join all threads back
	void* stat; //will hold exit code
	int ret = 0; //keep track if any thread failed
	int all = 0; //keep track if all failed
	for (i=0 ; i<num ; i++){
		stat = NULL; //see what this specific thread returned
		tmp = pthread_join(threads[i], &stat);
		if (tmp!=0 && tmp!=ESRCH){ //joined failed for other reason than thread does not exsist
			destroy();
			exit(1);
		}
		if (((long)stat)!=0 && (stat)!=PTHREAD_CANCELED){ //this thread returned 1 for failure.
			ret = 1;
			all++;
		}
	}
	if (all==num){ //all threads failed
		puts("All threads died :(");
		destroy();
		exit(1);
	}
	if (finished){ //This flag is raised by a SIGINT
		printf("Search stopped");
	}
	else{
		printf("Done traversing the sub-tree");
	}
	if (max.name==NULL){
		puts(" no dir processed yet :(");
	}
	else{
		printf(", directory %s has the largest files size of %lu bytes \n", basename(max.name), max.size);
	}
	destroy();
	exit(ret); //if we reached this all threads were already joined (no fear that this will terminate prematurely)
}

/*
 * Logic for thread tasks.
 * Notice that no data is allocated by this function, but:
 * it is responsible for the name string it dequeued (free it or pass it to the max struct).
 * Logic:
 * 	untill caceled or get flag that all threads are idle, dequeue a dir name
 * 	(dedque will block untill queue is not empty, note that thread is open to cancelation waiting for queue to fill)
 * 	call get_size to sum directory file sizes (this function also queues new dirs it encounters)
 * 	call change_max to compare this dir's size to current largest.
 * 	before continuing to next dir, check if we need to be canceled.
 *
 */
void* thread_do(void* i){
	pthread_testcancel();
	long serial = (long) i; //this thread's serial number
	int done = 0; //dequeue will change to 1 when all threads become idle and program needs to finish
	char* name = NULL;
	unsigned long size;
	CHECK_THREAD((pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL)),(stderr,"error checking cancel, thread %ld\n",serial));
	while (1){
		name = dequeue(&done);
		if ((name == NULL)){
			if (done){
				break;
			}
			else{ //error in dequeue
				alive[serial]=0; //update that this thread is dead
				__sync_fetch_and_sub(&total, 1); //update that there is one less thread
				fprintf(stderr,"error in dequeue for thread %ld \n",serial); //c, no resources to release
				return (void*)1;
			}
		}
		size = get_size(name, serial);
		printf("%s, files total size: %lu\n", name, size); //c, name will be freed on cleaner
		change_max(name,size,serial); //test if this dir is the largest so far, frees the unused
		name = NULL; //either freed by change max or is now owned by max struct
		CHECK_THREAD((pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL)),(stderr,"error checking cancel, thread %ld\n",serial));
		pthread_testcancel();
		CHECK_THREAD((pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL)),(stderr,"error checking cancel, thread %ld\n",serial));
	}
	return (void*)0;
}

/*
 * Goes over all files in directory "name", and sums their sizes.
 * directory sizes are not summed, but instead inserted into queue (except root and father pointers: "." "..")
 * This function contains cancellation points which are handled clean up function to close open dir and release directory name
 */
int get_size(char* name, long serial){
	int size = 0;
	struct stat info;
	struct dirent *entry;
	DIR *cur= NULL;
	CHECK_THREAD((!(cur = opendir(name))),(stderr,"error opening dir %s thread %ld\n",name,serial)); //c
	errno = 0; //Distinguish errors for dir
	while ((entry = readdir(cur)) != NULL){ //get files in dir
		//handle if current file is another dir
		if (entry->d_type == DT_DIR){
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0){
				continue;
			}
			else{
				CHECK_THREAD((enque(name,entry->d_name)),(stderr,"error adding dir %s to queue thread %ld\n",entry->d_name,serial)); //note this actualy adds new directory to dir
			}
		}
		//if a regular file
		else if (entry->d_type == DT_REG){
			CHECK_THREAD((fstatat(dirfd(cur), entry->d_name, &info, 0)),(stderr,"error getting stat info on file %s, thread %ld, errno %d\n",entry->d_name,serial,errno));
			size += info.st_size;
		}
	}
	CHECK_THREAD((errno!=0),(stderr,"error while readdir on %s, thread %ld errno %d \n",name,serial,errno));
	CHECK_THREAD(closedir(cur),(stderr,"error closing dir"));
	return size;
}

/*
 * Checks if directory "name" has the largest file sizes so far.
 * Frees the "loser" directory name (as it is no longer needed)
 * Note that as Macro Check_THREAD is used, in mutex error- this function will cleanup for thread and pthread_exit.
 * Only cancel point in this function is in the CHECK_THREAD function and means we already cleaned up and ready to exit.
 */
void change_max(char* name, int size, long serial){
	CHECK_THREAD(pthread_mutex_lock(&max_mutex),(stderr,"error acquiring max lock thread %ld",serial));
	if (max.size < size){
		if (max.name!=NULL){
			free(max.name);
		}
		max.name = name;
		max.size = size;
	}
	else{
		free(name);
	}
	CHECK_THREAD(pthread_mutex_unlock(&max_mutex),(stderr,"error releasing max lock thread %ld",serial));
}

/*
 * Takes a node out of the queue and returns a pointer to the string it contained.
 * Recieves a pointer to a flag "done" to signal that the returned value was 0 not due to an error (work is done)
 *
 * If the queue is empty, this thread will wait for cond var empty.
 * The is all threads but one are idle, this thread will not wait as well but wake every body up as work is done.
 *
 * This function contain one of two cancelable points in a threads life: we want all idle threads waiting for convar.
 * to be canceled (otherwise they might never wake). But if they just cancel they will leave the lock as.
 * To deal with this a cleanup function was pushed.
 *
 */
char* dequeue(int *done){
	char *name = NULL;
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); //DD
	if (pthread_mutex_lock(&q_mutex)){ //failed acquiring mutex
		fprintf(stderr,"error in dequeue mutex acquire");
		return NULL;
	}
	pthread_cleanup_push(clean_lock,&q_mutex); //DD
	while (EMPTY && !(ALL_IDLE)){
		//printf("going to sleep, already %d waiting out of %d threads\n",idle,total); //c
		idle++; //update counter that this thread is also idle, okay to increment since we are under a lock
		pthread_cond_wait(&empty, &q_mutex); //c
		idle--; //update counter that this thread is also idle, okay to decrement since we are under a lock
		//printf("woke, already %d waiting out of %d threads\n",idle,total);//c
	}
	pthread_cleanup_pop(0); //DD
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL); //DD
	if (EMPTY && LAST){ //all threads finished working
		pthread_cond_broadcast(&empty);
		idle = idle + 2; //so waking threads still know we are done (they decrement idle when they exit- this way they know a different thread finished)
	}
	if ((EMPTY && ALL_IDLE)){
		idle++; //to cancel decrement action going out of while loop
		if (pthread_mutex_unlock(&q_mutex)){
			fprintf(stderr,"error in dequeue mutex release"); //c (doesn't matter as means release failed)
			return NULL;
		}
		*done = 1; //signal caller we finished working
		return NULL;
	}
	if (q.head==NULL){
		pthread_mutex_unlock(&q_mutex);
		return NULL;
	}
	//done with all the Sh*t, now this is a normal dequeue assuming the queue is not empty, no cpoints until end of code
	node* tmp = q.head; //Guaranteed not to be NULL
	q.head = (q.head)->next;
	if EMPTY{
		q.tail = NULL;
	}
	if (pthread_mutex_unlock(&q_mutex)){
		destroy_node(tmp); //free node that was taken out.
		fprintf(stderr,"error in dequeue mutex release"); //cancelation point, but mutex failed to be freed anyway
		return NULL;
	}
	name = tmp->name;
	free(tmp); //free node that was taken out, note that we keep the allocated string
	return name;
}

/*
 * Adds a directory name to queue.
 * Receives a pointer to a string holding the name of wanted directory.
 * If queue was previously empty, will wake threads waiting on cond var.
 * Returns 0 on success, 1 otherwise.
 * Only cancellation points in this function are prints which are made when all resorces have already been freed
 */
int enque(char* dir, char* name){
	node* n = make_node(dir, name);
	if (n==NULL){
		return 1;
	}
	if (pthread_mutex_lock(&q_mutex)){ //failed acquiring mutex in enqueue
		destroy_node(n);
		fprintf(stderr,"error in enqueue mutex acquire");
		return 1;
	}
	if EMPTY{
		q.head = n;
		q.tail = n;
		if (pthread_cond_signal(&empty)){
			pthread_mutex_unlock(&q_mutex);//release taken lock
			fprintf(stderr,"error in signaling");
			return 1;
		}
	}
	else{
		(q.tail)->next = n;
		q.tail = n;
	}
	if (pthread_mutex_unlock(&q_mutex)){
		fprintf(stderr,"error in enqueue mute release");
		return 1;
	}
	return 0;
}

/*
 * Creates a list node containing directory name.
 * On success returns pointer to node, on failure returns NULL
 * Names are probably part of structs, so we will allocate new space for strings and copy them.
 * There are no cancellation points in this function, except prints in errors which result in termination any way
 */
node* make_node(char* path, char* dir){
	node* n = calloc(1,sizeof(node));
	if (n==NULL){
		fprintf(stderr,"error in allocating new node\n");
		return NULL;
	}
	int len = strlen(path) + strlen(dir) + 1; //node will contain full path (directory + / + direc)
	n->name = malloc((len+1)*sizeof(char)); //+1 to make room for null terminator as well
	if (n->name == NULL){
		free(n);
		fprintf(stderr,"error in allocating new string - make node\n");
		return NULL;
	}
	strncpy(n->name,path,strlen(path)); //copy path name
	(n->name)[strlen(path)] = '/'; //add / seperator
	strncpy((n->name + strlen(path) + 1),dir,strlen(dir)+1); //copy dir name + null terminator
	return n;
}

/*
 * Frees all allocated data associated with this node.
 * note that any nodes pointed by this node will still be allocated.
 */
void destroy_node(node* n){
	free(n->name);
	free(n);
}

/*
 * Frees all allocated data in queue.
 * Used when a sigint is recieved and we exit when queue is not empty
 */
void destroy_q(){
	node* cur, *nxt;
	cur = (q.head);
	while (cur != NULL){
		nxt = cur->next;
		destroy_node(cur);
		cur = nxt;
	}
}

/*
 * Initializes all resources (locks, condvar etc) and data structures for process.
 * Separated from main in order to improve readability.
 */
int init(int num){
	max.size = 0; //no need for atomicity as no concurancy at this point
	max.name = NULL;
	threads = (pthread_t*) calloc(num , sizeof(pthread_t));
	if (threads == NULL){
		fprintf(stderr,"error in allocating threads array\n");
		return 1;
	}
	alive = (char*) calloc(num , sizeof(char));
	if (alive == NULL){
		free(threads);
		fprintf(stderr,"error in allocating threads array\n");
		return 1;
	}
	if (pthread_mutex_init(&max_mutex, NULL)){ //init lock for max struct
		free(alive);
		free(threads);
		fprintf(stderr,"Failure initializing max lock\n");
		return 1;
	}
	if (pthread_mutex_init(&q_mutex, NULL)){
		free(alive);
		free(threads);
		pthread_mutex_destroy(&max_mutex);
		fprintf(stderr,"Failure initializing q lock\n");
		return 1;
	}
	if (pthread_cond_init (&empty, NULL)){
		free(alive);
		free(threads);
		pthread_mutex_destroy(&max_mutex);
		pthread_mutex_destroy(&q_mutex);
		fprintf(stderr,"Failure conditional variable\n");
		return 1;
	}
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
		fprintf(stderr,"Failure assigning sig handler\n");
		return 1;
	}
	return 0;
}

/*
 * Releases all resources allocated by main function.
 * Separated from main for modularity and to decrease code blow up
 */
void destroy(){
    free(threads);
    free(alive);
	pthread_mutex_destroy(&q_mutex);
	pthread_mutex_destroy(&max_mutex);
	pthread_cond_destroy(&empty);
	destroy_q();
	if (max.name!=NULL){
		free(max.name);
	}
}

/*
 * Custom sig handler for SIGINT
 * Sends cancellation orders to all live threads, and updates a flag indicating that the search has stopped (used by main
 * for output serving).
 */
void sig_handler(int signum, siginfo_t *info, void *ptr){
	if (finished){ //so sigint won't be caught more than once
		return;
	}
	printf("\n\n\n\n SIGINT caught %ld. Wrapping it up.\n \n\n\n\n",pthread_self());
	__sync_fetch_and_add(&finished, 1); //threads will know to wrap it up
	for (long i=0; i<num ; i++){
		if (alive[i]!=0){
			pthread_cancel(threads[i]); //probably will return 3, as cancellation is delayed
		}
	}
	return;
}

/*
 * Cleanup function pushed to release lock when a thread gets canceled waiting on condition.
 */
void clean_lock(void* lock){
	pthread_mutex_unlock(lock);
	//printf("cleaned lock by thread %ld\n",pthread_self());
}

