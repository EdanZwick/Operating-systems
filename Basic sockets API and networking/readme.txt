Basic program to exercise socket API and networking.
Client side: 
   Recieves host (name or ip), port number and legnth as command line arguments.
   Opens a connection with host and sends *length* random bytes to host.
   Waits for a respond from server containing the number of printable (ascii range: ' '-'~') bytes sent.
   Prints the response to standart output.
    
Server side:
   Receives a port number as a command line argument.
   Starts accepting connections on that port, dedicating a new thread per connection.
   After SIGINT is received, the number of times each printable char was received (from all connections) is printed.
 
 	 Server-client protocol:
 	 Client connects and sends the number of total bytes (N) in message (as an unsigned int).
 	 Server "reads" until N bytes are recieved.
 	 Server responeds with total number of printable bytes (again as an unsigned int).
 
   The data structure used for counting chars is an array where each slot holds the number times the corresponding char
 	 was received. Synchronization between server threads is kept by updating the slots using only atomic functions.
