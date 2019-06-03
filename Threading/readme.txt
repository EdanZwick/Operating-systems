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
