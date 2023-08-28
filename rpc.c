#define _POSIX_C_SOURCE 200112L
#define NONBLOCKING

#include "rpc.h"
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <ctype.h>
#include <endian.h>
#include <sys/select.h>
#include <pthread.h>

#define _DEFAULT_SOURCE 
#define __USE_BSD
#include <endian.h>


#define MAX_INT_LEN 4
#define INIT_REGISTER_SIZE 1
#define MAX_NAME_LEN 1000
#define MAX_LEN	100000
#define NUM_THREAD 20

#define CALL 2
#define FIND 1

pthread_t tid[NUM_THREAD];
pthread_mutex_t lock_call, lock_find;

/* FUNCTION PROTOTYPES */
int create_listening_socket(char* service);
void *decide_func(void *srv);
void* find(void *srv);
void* call(void *srv);
extern char* strdup(const char*);
void send_string(int socket, char* string);
int read_string(int socket, int max_len, char* string);
void send_int(int socket, int num);
int read_int(int socket, fd_set *masterfd);
uint64_t ntohll(uint64_t *num);
uint64_t htonll(uint64_t *num);
void send_data1(int socket, int64_t num);
int64_t read_data1(int socket, fd_set *masterfd);
void send_stream(int socket, size_t len, void* stream);
void read_stream(int socket, size_t len, char* result);

/* ======================================================================== */

struct rpc_server {
    int socketfd;
	// stores the number of functions registered
	int num_registered;
	// stores all the functions registered
	char **registered;
	int curr_register_size;
	rpc_handler *handlers;
};

rpc_server *rpc_init_server(int port) {
    /* return rpc_server on success, NULL on failure */

	char s_port[MAX_INT_LEN+1];
	sprintf(s_port, "%d", port);
	s_port[MAX_INT_LEN] = '\0';

	rpc_server *server = (rpc_server*)malloc(sizeof(rpc_server));
	assert(server);
    server->socketfd = create_listening_socket(s_port);
	if (listen(server->socketfd, 10) < 0) {
		return NULL;
	}

	// initialise registered name and functions array
	server->num_registered = 0;
	server->registered = (char**)malloc(INIT_REGISTER_SIZE * sizeof(char*));
	assert(server->registered);
	server->curr_register_size = INIT_REGISTER_SIZE;
	server->handlers = (rpc_handler*)malloc(INIT_REGISTER_SIZE * sizeof(rpc_handler));
	assert(server->handlers);

	return server;
}

int rpc_register(rpc_server *srv, char *name, rpc_handler handler) {
    /* return an ID for this handler on success, and -1 on failure */
	if (handler != NULL || strlen(name) > MAX_NAME_LEN) {

		// check if the function name exists
		for (int i = 0; i < srv->num_registered; i++) {
			if (strcmp(srv->registered[i], name) == 0) {
				// if the name exist, return the index
				srv->handlers[i] = handler;
				return i;
			}
		}

		// if it is a new function, add into the array
		if (srv->num_registered >= srv->curr_register_size) {
			// if the number of functions exceeds the current array size, extend it
			srv->curr_register_size *= 2;
			srv->registered = (char**)realloc(srv->registered, srv->curr_register_size * sizeof(char*));
			assert(srv->registered);
			srv->handlers = (rpc_handler*)realloc(srv->handlers, srv->curr_register_size * sizeof(handler));
			assert(srv->handlers);
		}
		srv->registered[srv->num_registered] = strdup(name);
		srv->handlers[srv->num_registered] = handler;
		srv->num_registered += 1;

		return srv->num_registered;
	}
    return -1;
}

// an alias for server to pass into threads
typedef struct {
	rpc_server *srv;
	int fd;
	int op;
	fd_set *masterfds;
	char* buffer_name;
	int index;
	rpc_data *data;
	int *t_flag;
} server_alias;

void rpc_serve_all(rpc_server *srv) {

	int newsockfd;
	struct sockaddr_storage client_addr;
	socklen_t client_addr_size;
	int t_flag[NUM_THREAD] = {0};

	// initialise an active fd set
	fd_set masterfds;
	FD_ZERO(&masterfds);
	FD_SET(srv->socketfd, &masterfds);
	// record the maximum socket number
	int maxfd = srv->socketfd;

	pthread_mutex_init(&lock_call, NULL);
	pthread_mutex_init(&lock_find, NULL);
	
	while(1) {
		// monitor file descripters
		fd_set readfds = masterfds;
		if (select(FD_SETSIZE, &readfds, NULL, NULL, NULL) < 0) {
			perror("select");
			exit(EXIT_FAILURE);
		}

		// loop all possible descripter
		for (int fd = 0; fd <= maxfd; ++fd) {			
			// determine if the current file descripter is active
			if (!FD_ISSET(fd, &readfds)) {
				continue;
			}

			// create new socket if there is new incoming connection request
			if (fd == srv->socketfd) {
				// Accept a connection - blocks until a connection is ready to be accepted
				// Get back a new file descriptor to communicate on
				client_addr_size = sizeof client_addr;
				newsockfd =
					accept(srv->socketfd, (struct sockaddr*)&client_addr, &client_addr_size);
				if (newsockfd < 0) {
					perror("accept");
					exit(EXIT_FAILURE);
				} else {
					// add the socket to the set
					FD_SET(newsockfd, &masterfds);
					//update the maximum tracker
					if (newsockfd > maxfd) 
						maxfd = newsockfd;
				}
			} else {

				// if the fd is active and the corresponding client has no 
				// threads running, create a new thread
				if (FD_ISSET(fd, &readfds)) {
					if (t_flag[fd] == 0) {
						server_alias *srv_a = malloc(sizeof(server_alias));
						srv_a->fd = fd;
						srv_a->srv = srv;
						srv_a->masterfds = &masterfds;
						srv_a->t_flag = &t_flag[fd];
						pthread_create(&tid[fd], NULL, &decide_func, srv_a);
						t_flag[fd] = 1;
					} 
				}
			}
		}
	}
	
	for (int i = 0; i < NUM_THREAD; i++) {
		pthread_join(tid[i], NULL);
	}
	pthread_mutex_destroy(&lock_call);
	pthread_mutex_destroy(&lock_find);
	close(srv->socketfd);
	close(newsockfd);
}

struct rpc_client {
    int socketfd;
};

struct rpc_handle {
	int index;
	rpc_handler *handler;
};

rpc_client *rpc_init_client(char *addr, int port) {

	struct addrinfo hints, *servinfo;
	int s;

	char s_port[MAX_INT_LEN+1];
	sprintf(s_port, "%d", port);
	s_port[MAX_INT_LEN] = '\0';

	rpc_client *client = (rpc_client*)malloc(sizeof(rpc_client));

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET6;
	hints.ai_socktype = SOCK_STREAM;
	s = getaddrinfo(addr, s_port, &hints, &servinfo);
	if (s != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
		exit(EXIT_FAILURE);
	}

	struct addrinfo *rp;
	for (rp = servinfo; rp != NULL; rp = rp->ai_next) {
		client->socketfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (client->socketfd == -1) continue;
		if (connect(client->socketfd, rp->ai_addr, rp->ai_addrlen) != -1) break;
		close(client->socketfd);
	}
	freeaddrinfo(servinfo);
	if (rp == NULL) {
		fprintf(stderr, "client: failed to connect\n");
		exit(EXIT_FAILURE);
	}
	return client;
}

rpc_handle *rpc_find(rpc_client *cl, char *name) {

	rpc_handle *handle = (rpc_handle*)malloc(sizeof(rpc_handle));
	assert(handle);
	handle->index = -1;
	send_int(cl->socketfd, FIND);

	// send the name to server
	send_string(cl->socketfd, name);

	// read response from server 
	handle->index = read_int(cl->socketfd, NULL);

	if (handle->index >= 0) {
		return handle; 
	} else return NULL;
}

rpc_data *rpc_call(rpc_client *cl, rpc_handle *h, rpc_data *payload) {

	// if there are bad data presend, stop the call
	if (cl == NULL || h == NULL || payload == NULL) {
		return NULL;
	} else if (payload->data2_len != 0 && payload->data2 == NULL) {
		return NULL;
	} else if (payload->data2_len == 0 && payload->data2 != NULL) {
		return NULL;
	}

	send_int(cl->socketfd, CALL);
	
	// send index, data1, data2_len and data2 to server
	send_int(cl->socketfd, h->index);
	send_data1(cl->socketfd, (int64_t)payload->data1);
	send_int(cl->socketfd, payload->data2_len);
	if (payload->data2_len > 0) {
		send_stream(cl->socketfd, payload->data2_len, payload->data2);
	}

	// receive data1, data2_len and data2 from server
	rpc_data *data_in = (rpc_data*)malloc(sizeof(rpc_data));
	data_in->data1 = (int)read_data1(cl->socketfd, NULL);
	int len = read_int(cl->socketfd, NULL);

	if (len > 0) {
		// if data2_len is present, read the stream
		data_in->data2_len = len;
		char buffer_receive[data_in->data2_len+1];
		read_stream(cl->socketfd, data_in->data2_len, buffer_receive);
		data_in->data2 = strdup(buffer_receive);
	} else if (len == -1) {
		// if data2_len indicates an error on server side, stop the call
		return NULL;
	} else {
		// if data2_len is 0, do not read the stream
		data_in->data2 = NULL;
	}

    return data_in;
}

void rpc_close_client(rpc_client *cl) {
	if (cl != NULL) {
		send_int(cl->socketfd, 0);
		free(cl);
		cl = NULL;
	}
}

void rpc_data_free(rpc_data *data) {
    if (data == NULL) {
        return;
    }
    if (data->data2 != NULL) {
        free(data->data2);
		data->data2 = NULL;
    }
    free(data);
	data = NULL;
}


/* ======================================================================== */

/* Create a listening socket for the server */
int create_listening_socket(char* service) {
	int re, s, sockfd;
	struct addrinfo hints, *res;

	// Create address we're going to listen on (with given port number)
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET6;      // IPv6
	hints.ai_socktype = SOCK_STREAM; // Connection-mode byte streams
	hints.ai_flags = AI_PASSIVE;     // for bind, listen, accept
	// node (NULL means any interface), service (port), hints, res
	s = getaddrinfo(NULL, service, &hints, &res);
	if (s != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
		exit(EXIT_FAILURE);
	}

	// Create socket
	struct addrinfo *p;
	// loop over the list of responses from getaddrinfo()
	for (p = res; p != NULL; p = p->ai_next) {
		if (p->ai_family == AF_INET6 && (
			sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)
		) < 0) {
			exit(EXIT_FAILURE);
		}
	}

	// Reuse port if possible
	re = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &re, sizeof(int)) < 0) {
		printf("entered setsockopt.\n");
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
	// Bind address to the socket
	int enable = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
	if (bind(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
		printf("entered bind error\n");
		perror("bind");
		exit(EXIT_FAILURE);
	}
	freeaddrinfo(res);
	return sockfd;
}

/* to decide which function to run, and call the corresponding function */
void *decide_func(void *srv) {
	
	server_alias* server = (server_alias*) srv;
	pthread_mutex_lock(&lock_find);

	// reads the operation
	server->op = read_int(server->fd, server->masterfds);
	pthread_mutex_unlock(&lock_find);

	if (server->op == FIND) {
		// call FIND 
		find((void*)srv);
	} else if (server->op == CALL) {
		// call CALL
		call((void*)srv);
	} 

	*server->t_flag = 0;

	// if there is no more operation, free the server alias
	if (server->op == 0) {
		FD_CLR(server->fd, server->masterfds);
		if (srv) {
			free(srv);
			srv = NULL;
		}
	}
	return NULL;
}

/* find function */
void* find(void *srv) {
	
	server_alias *server = (server_alias*) srv;
	char buffer_name[MAX_NAME_LEN+1];

	pthread_mutex_lock(&lock_find);
	read_string(server->fd, MAX_NAME_LEN+1, buffer_name);
	pthread_mutex_unlock(&lock_find);

	// search for the function from the functions array
	int find_flag = -1;
	for (int j = 0; j < server->srv->num_registered; j++) {
		if (strcmp(server->srv->registered[j], buffer_name) == 0) {
			// if the function is found, send back its index
			pthread_mutex_lock(&lock_find);
			send_int(server->fd, j);
			pthread_mutex_unlock(&lock_find);
			find_flag = j;
			break;
		}
	}
	if (find_flag == -1) {
		// if not found, send back -1
		pthread_mutex_lock(&lock_find);
		send_int(server->fd, -1);
		pthread_mutex_unlock(&lock_find);
	}
	return NULL;
}

/* call function */
void* call(void *srv) {

	server_alias *server = (server_alias*) srv;
	server->index = read_int(server->fd, NULL);

	// read data1, data2_len and data2
	char buffer_data[MAX_LEN+1];
	server->data = malloc(sizeof(rpc_data));
	pthread_mutex_lock(&lock_call);
	server->data->data1 = (int)read_data1(server->fd, NULL);
	server->data->data2_len = read_int(server->fd, NULL);
	if (server->data->data2_len > 0) {
		// if the length of data2 is more than 100000, produce an error
		/*
		if (server->data->data2_len > MAX_LEN) {
			fprintf(stderr, "Overlength error\n");
			return NULL;
		}
		*/
 		server->data->data2 = &buffer_data[server->data->data2_len+1];
		read_stream(server->fd, server->data->data2_len, server->data->data2);
	} else {
		server->data->data2 = NULL;
	}
	pthread_mutex_unlock(&lock_call);

	// call the corresponding handler function
	rpc_handler handler = server->srv->handlers[server->index];
	rpc_data *data_out = (*handler)(server->data);
	pthread_mutex_lock(&lock_call);

	if ((data_out == NULL) || (data_out->data2_len == 0 && data_out->data2 != NULL) || (data_out->data2_len != 0 && data_out->data2 == NULL)) {
		// if the result produced by server is bad, send -1 to client's 
		// data2_len field to indicate an error
		send_int(server->fd, 1);
		send_int(server->fd, -1);

	} else {
		// send data1, data2_len and data2 to client
		send_data1(server->fd, (int64_t)data_out->data1);
		send_int(server->fd, data_out->data2_len);
		if (data_out->data2_len > 0) {
			send_stream(server->fd, data_out->data2_len, data_out->data2);
		}
	}

	pthread_mutex_unlock(&lock_call);
	free(data_out);
	data_out = NULL;
	return NULL;
}

/* sent a string with a header indicating length */
void send_string(int socket, char* string) {

	// Encode the first 4 bytes to be the stirng length 
	int len = strlen(string);
	char buffer[MAX_INT_LEN+len];
	uint32_t* val = (uint32_t*)&buffer;
	*val = htonl(len);

	strcpy(&buffer[MAX_INT_LEN], string);
	if (write(socket, buffer, MAX_INT_LEN+len) < 0) {
		close(socket);
		perror("socket");
		exit(EXIT_FAILURE);
	}
}

/* read a string with a header indicating length */
int read_string(int socket, int max_len, char* string) {

	// read the length of the string first 
	char buffer[max_len];
	if (read(socket, buffer, MAX_INT_LEN) < 0) {
		perror("read");
		exit(EXIT_FAILURE);
	}
	int len = (int)ntohl(*(uint32_t*)&buffer);
	if (read(socket, buffer, len) < 0) {
		perror("read");
		exit(EXIT_FAILURE);
	}

	buffer[len] = '\0';
	strcpy(string, buffer);
	return len;
}

/* send an integer using network byte ordering */
void send_int(int socket, int num) {

	char buffer[MAX_INT_LEN];
	uint32_t* val = (uint32_t*)&buffer;
	*val = htonl(num);
	if (write(socket, buffer, MAX_INT_LEN) < 0) {
		close(socket);
		perror("socket");
		exit(EXIT_FAILURE);
	}
}

/* read an integer using network byte ordering */
int read_int(int socket, fd_set *masterfds) {

	char buffer[MAX_INT_LEN];
	int n = read(socket, buffer, MAX_INT_LEN);

	if (n == MAX_INT_LEN) {
		return (uint32_t)ntohl(*(uint32_t*)&buffer);
	} else if (n <= 0) {
		close(socket);
		FD_CLR(socket, masterfds);
	}
	return -1;
}

/* send a stream of data given a specified size */
void send_stream(int socket, size_t len, void* stream) {

	if (stream != NULL) {
		char buffer[len+1];
		strcpy(buffer, (char*)stream);
		buffer[len] = '\0';

		if (write(socket, buffer, len+1) < 0) {
			close(socket);
			perror("socket");
			exit(EXIT_FAILURE);
		}
	}
}

/* read a stream of data given a specified size */
void read_stream(int socket, size_t len, char* result) {

	char buffer[len+1];
	if (read(socket, buffer, len+1) < 0) {
		perror("read");
		exit(EXIT_FAILURE);
	}

	strcpy(result, buffer);
	result[len] = '\0';
}

/* send data1 using network byte ordering */
void send_data1(int socket, int64_t num) {

	char buffer[MAX_INT_LEN*2];
	int64_t* val = (int64_t*)&buffer;
	*val = htonll((uint64_t*)&num);

	if (write(socket, buffer, MAX_INT_LEN*2) < 0) {
		close(socket);
		perror("socket");
		exit(EXIT_FAILURE);
	}
}

/* read data1 using network byte ordering */
int64_t read_data1(int socket, fd_set *masterfds) {

	char buffer[MAX_INT_LEN*2];
	int n = read(socket, buffer, MAX_INT_LEN*2);

	if (n == MAX_INT_LEN*2) { 
		return (int64_t)ntohll((uint64_t*)&buffer);
	} else if (n <= 0) {
		close(socket);
		FD_CLR(socket, masterfds);
	}
	return -1;
}

/* Converts network byte order to host byte order for 64bit int */
uint64_t ntohll(uint64_t *num) {
    uint64_t rval;
    uint8_t *val = (uint8_t *)&rval;

    val[0] = *num >> 56;
    val[1] = *num >> 48;
    val[2] = *num >> 40;
    val[3] = *num >> 32;
    val[4] = *num >> 24;
    val[5] = *num >> 16;
    val[6] = *num >> 8;
    val[7] = *num >> 0;
    return rval;
}

/* Converts host byte order to network byte order for 64bit int */
uint64_t htonll(uint64_t *num) {
	return (ntohll(num));
}