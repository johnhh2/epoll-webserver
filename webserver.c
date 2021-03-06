#include "server_helpers.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
//#include <netinet/in.h>
#include <arpa/inet.h>
#include <magic.h>
#include <libconfig.h>

#define BACKLOG 10
#define EVENT_BUFFER 100

//default
#define DEFAULT_TIMEOUT_MS 1000

typedef struct request_info request_info;

// main functions
void init_server();
void accept_connections();
void add_client(int fd, char *ip);
void remove_client(int fd);
int handle_request(int fd);

// signal functions
void acknowledge_sigpipe(int);
void graceful_exit(int);

// handle_request helper functions
int get_header(request_info *);
verb check_verb(char *header);
int v_unknown(request_info *);
int get(request_info *);
int put(request_info *);
int send_status(int fd, int status, struct request_info *);
int send_status_n(int fd, int status, struct request_info *, size_t content_length);
int send_list(int fd, char *path, struct request_info *);
int send_error(int fd, int status, struct request_info *);
void set_mime_type(char *path, struct request_info *);
void parse_config(config_t *cf);

//Constants
static char *HTML_HEADER = "<!DOCTYPE html><html><head></head><body>";
static char *HTML_FOOTER = "</body></html>";

char *status_desc[510];

//Config settings
static const char *CONFIG_FILE = "/etc/epoll-webserver/server.conf";

static const char *DEFAULT_LOG_FILE = "/etc/epoll-server/log.txt";
static char *DEFAULT_SECURITY_HEADERS = "Cache-Control: private, max-age=0\n"
				"X-Frame-Options: SAMEORIGIN\n"
				"X-XSS-Protection: 1\n\n";
				//"X-Content-Type-Options: nosniff\n\n";

char *port = NULL;
static int max_file_size = 0;
static int timeout_ms = 0;
char *root_site = NULL;
char *security_headers = NULL;
FILE *http_log = NULL;

//Server info
static volatile int epollfd;
static volatile int server_socket;
struct request_info *client_requests[100];

struct request_info {
	struct epoll_event *event;
	char *ip;

	verb req_type;
	size_t stage;
	size_t progress;

	char *request_h; //header
	char *response_h;
	char *body;

	size_t range_start;
	size_t range_end;
	
	const char *mime_type;
};

void load_status_codes() {
	status_desc[200] = "OK";
	status_desc[204] = "No Content";
	status_desc[400] = "Bad Request";
	status_desc[401] = "Unauthorized";
	status_desc[403] = "Forbidden";
	status_desc[404] = "Not Found";
	status_desc[405] = "Method Not Allowed";
	status_desc[413] = "Payload Too Large";
	status_desc[414] = "Too Long";
	status_desc[431] = "Request Header Fields Too Large";
}

magic_t magic;
static char *MAGIC_FILE = "/usr/local/misc/magic.msc";

//TODO: Make temporary directory with mkdtemp("XXXXX") for gzip compression and probably more
//actually, just get the stdout of a forked gzip compression or php execution

//TODO: Fix block on read

void print_usage() {
	puts("Usage:\t./server");
	puts("Please set port and webserver_root in the server.conf (i.e. port = \"80\")");
	puts("Other configuration options:");
	puts("\tlog_file, security_headers, max_file_size, timeout_ms");
	exit(0);
}

//initialize server and poll for requests
int main(int argc, char **argv) {

	//Read config
	config_t cfg, *cf;
	cf = &cfg;
	config_init(cf);
	parse_config(cf);

	if (argc > 1) {
		print_usage();
	}

	load_status_codes();

	//load magiclib
	magic = magic_open(MAGIC_MIME_TYPE);
	magic_load(magic, MAGIC_FILE);
	magic_compile(magic, MAGIC_FILE);

	//signal handling
	signal(SIGINT, graceful_exit);
	signal(SIGPIPE, acknowledge_sigpipe);

	//start server
	init_server();
	LOG("Server Initialized on port %s\n", port);

	//mark file descriptors as non-blocking
	int flags = fcntl(server_socket, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(server_socket, F_SETFL, flags);
	//start epolling
	epollfd = epoll_create(1);
	LOG("Polling for requests\n");
	while (1) {
		accept_connections();
		
		struct epoll_event array[EVENT_BUFFER];

		//Get events
		int num_events = epoll_wait(epollfd, array, EVENT_BUFFER, timeout_ms);
		if (num_events == -1) {
			perror("epoll_wait");
			graceful_exit(0);
		}

		//Handle events
		for (int i = 0; i < num_events; i++) {
			int fd = array[i].data.fd;
			int event = array[i].events;

			if (event & EPOLLIN) {

				LOG("Working on request for %d\n", fd);

				int status = handle_request(fd); //process request
				LOG("Status for %d: %d\n", fd, status);

				if (status > 0) { //remove client on success, sigpipe, or error
					remove_client(fd);
				}
			}
			if (event & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
				remove_client(fd);
			}
		}
	}
}

//add client to epoll and the requests array
void add_client(int fd, char *ip) {
	struct epoll_event *ev = calloc(1, sizeof(struct epoll_event));
	ev->events = EPOLLIN | EPOLLET;
	ev->data.fd = fd;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, ev);

	if (client_requests[fd] == NULL) {
		struct request_info *req_info = calloc(1, sizeof(struct request_info));
		req_info->event = ev;
		req_info->ip = ip;

		client_requests[fd] = req_info;		
		LOG("Added client %d\n", fd);

	} else {
		LOG("add_client conflict on socket %d\n", fd);
	}
}

//remove client from epoll and the requests array
void remove_client(int fd) {

	if (client_requests[fd]) {
		epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);

		struct request_info *req_info = client_requests[fd];
		free(req_info->event);
		if (req_info->request_h) {
			free(req_info->request_h);
		}
		if (req_info->response_h) {
			free(req_info->response_h);
		}

		free(req_info);

		client_requests[fd] = NULL;

		shutdown(fd, SHUT_RDWR);
		close(fd);

		LOG("Removed client %d\n", fd);
	} else { //for debugging
		LOG("Tried to remove non-existent key for fd %d from client_requests\n", fd);
	}
}

//stage 0: read in header
//stage 1+: process command
//returns 0 on block, 1 on success, 2 on sigpipe/error
int handle_request(int fd) {
	struct request_info *req_info = client_requests[fd];
	errno = 0; //just in case for now

	//Stage 0: Read Header
	if (req_info->stage == 0) {
		int ret;
		if ((ret = get_header(req_info)) != 1) {
			return ret;
		}
		req_info->stage = 1;
	}

	//log
	if (http_log != NULL) {
		int header_sample_len = (int)(strchr(req_info->request_h, '\n') - req_info->request_h) - 1;
		char message[256];

		sprintf((char*)&message, "[%s] \"%.*s\"\n", req_info->ip, header_sample_len, req_info->request_h); 

		int message_len = strlen((char*)&message);

		fwrite(&message, message_len, 1, http_log);
		LOG("Logged: %.*s", message_len, message); 
	}

	LOG("Req enum: %d\n", req_info->req_type);
	if (req_info->req_type == 0) {
		req_info->req_type = check_verb(req_info->request_h);
	}

	//Stage 1+: Process Request
	if (req_info->req_type == V_UNKNOWN) {
		return v_unknown(req_info);

	} else if (req_info->req_type == GET || req_info->req_type == HEAD) {
		return get(req_info);

	//else if (req_info->req_type == PUT) {
	//  return put(req_info);

	} else { //Verb not implemented/allowed send status 405
		if (req_info->response_h == NULL) {

			//Allocate space for the response header
			if (req_info->response_h == NULL) {
				req_info->response_h = calloc(1, MAX_HEADER_SIZE);
			}

			return send_error(fd, 405, req_info);
		}
	}

	return 1; //successfully reached the end
}

//return the type of request indicated by the beginning of the header
verb check_verb(char *header) {

	if (strncmp(header, "GET ", 4) == 0) {
		return GET;
	} else if (strncmp(header, "HEAD ", 5) == 0) {
		return HEAD;
	} else if (strncmp(header, "POST ", 5) == 0) {
		return POST;
	} else if (strncmp(header, "PUT ", 4) == 0) {
		return PUT;
	} else if (strncmp(header, "DELETE ", 7) == 0) {
		return DELETE;
	} else if (strncmp(header, "CONNECT ", 8) == 0) {
		return CONNECT;
	} else if (strncmp(header, "OPTIONS ", 8) == 0) {
		return OPTIONS;
	} else if (strncmp(header, "TRACE ", 6) == 0) {
		return TRACE;
	}
	return V_UNKNOWN;
}

//initialize server
void init_server() {
	server_socket = socket(AF_INET, SOCK_STREAM, 0);

	int optval = 1;
	if (setsockopt(server_socket, SOL_SOCKET, SO_BROADCAST || SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
		perror("setsockopt");
		graceful_exit(0);
	}

	//set hints
	struct addrinfo hints, *infoptr;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	//get addrinfo for host from hints
	int result = getaddrinfo("0.0.0.0", port, &hints, &infoptr);
	if (result) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
		graceful_exit(0);
	}

	if (bind(server_socket, infoptr->ai_addr, infoptr->ai_addrlen) == -1) {
		perror("Bind");
		graceful_exit(0);
	}

	if (listen(server_socket, BACKLOG) == -1) {
		perror("Listen");
		graceful_exit(0);
	}

	LOG("Listening on file descriptor %d, port %s\n", server_socket, port);

	freeaddrinfo(infoptr);
}

//accept pending connections
void accept_connections() {

	int fd = 0;
	struct sockaddr client_addr;
	socklen_t client_addr_len = sizeof(client_addr);
	while ((fd = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len)) > 0) {

		LOG("Found client\n");

		if (fd == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
			perror("accept");
			LOG("Failed to connect to a client\n");
		} else if (fd >= 0) {
			struct sockaddr_in *client_addr_in = (struct sockaddr_in *)&client_addr;
			add_client(fd, inet_ntoa(client_addr_in->sin_addr));
			accept_connections();
			LOG("Accepted %s on file descriptor %d\n", inet_ntoa(client_addr_in->sin_addr), fd);
		} else {
			errno = 0;
		}
	}
}

void acknowledge_sigpipe(int arg) {
	LOG("SIGPIPE!\n");
}

void graceful_exit(int arg) {

	//remove any existing clients
	for (int i=0; i < 100; i += 1) {
		if (client_requests[i] != NULL) {
			remove_client(i);
		}
	}

	close(server_socket);

	//close log
	if (http_log != NULL) {
		fclose(http_log);
	}

	//close magic
	if (magic != NULL) {
		magic_close(magic);
	}

	if (security_headers != NULL) {
		free(security_headers);
	}

	if (port != NULL) {
		free(port);
	}

	if (root_site != NULL) {
		free(root_site);
	}

	exit(0);
}

int get_header(request_info *req_info) {
	int fd = req_info->event->data.fd;

	LOG("\tStage 0, %zu prior progress\n", req_info->progress);

	if (req_info->request_h == NULL) {
		LOG("\tHeader buffer allocated\n");
		req_info->request_h = calloc(1, MAX_HEADER_SIZE*sizeof(char));
	}

	ssize_t read_status = read_header(fd, req_info->request_h + req_info->progress, MAX_HEADER_SIZE - req_info->progress);
	LOG("\tRead status: %zd\n", read_status);

	//is header too long?
	if (read_status == -1) {
		return send_error(fd, 413, req_info);
	}

	//Did we make progress?
	if (read_status > 0) {
		req_info->progress += read_status;
	}

	LOG("Header: %s\n", req_info->request_h);

	//Return on block/error, otherwise go to next stage
	if (errno == EWOULDBLOCK || errno == EAGAIN) {
		LOG("Read blocked!\n");
		//Resume request later
		return 0;
	} else if (errno == SIGPIPE) {
		LOG("Sigpipe on %d\n", fd);
		//Ignore request
		return 3;
	} else if (errno != 0) { //SIGPIPE or error
		LOG("Error reading header\n");
		//Ignore request
		return 3;
	}

	//Multiple checks for malformed/long requests or missing headers

	//If empty
	if (strlen(req_info->request_h) == 0) {
		LOG("Empty request, moving on.\n");
		return 1;
	}

	void *path_start = strchr(req_info->request_h, ' ') + 1;
	void *protocol_start = strchr((char*)path_start, ' ') + 1;
	void *header_end = strstr(req_info->request_h, "\n\n");

	//Malformed
	if (path_start == NULL || protocol_start == NULL || header_end
			|| strncmp(protocol_start, "HTTP/", 5) != 0  ) {

		return send_error(fd, 400, req_info);
	}
	
	//Too Long
	if ((int)(protocol_start - path_start - 1) > MAX_PATHNAME_SIZE) {
		LOG("Path length of %d exceeded limit of %d\n",
				(int)(protocol_start - path_start - 1), MAX_PATHNAME_SIZE);

		return send_error(fd, 414, req_info);
	}

	//Host header
	if (strstr(req_info->request_h, "Host:") == NULL) {
		return send_error(fd, 400, req_info);
	}

	//Check range
	req_info->range_start = 0;
	req_info->range_end = 0;

	//Scan in range
	if (strstr(req_info->request_h, "Range:") != NULL) {
		sscanf(req_info->request_h, "Range: bytes=%zu-%zu\n", &req_info->range_start, &req_info->range_end);
	}
	

	LOG("completed reading header!\n");
	req_info->stage = 1;
	req_info->progress = 0;

	return 1;
}
int v_unknown(request_info *req_info) {
	int fd = req_info->event->data.fd;

	return send_error(fd, 400, req_info);
}

int get(request_info *req_info) {
	int fd = req_info->event->data.fd;

	// path = root_site .. path (index.html if needed)
	char path[MAX_PATHNAME_SIZE + strlen(root_site) + 1];
	memcpy(path, root_site, strlen(root_site));

	//Scan path
	if (sscanf(req_info->request_h, "%*s %s", path + strlen(root_site)) != 1) {
		return send_error(fd, 400, req_info);
	}

	//Append request path
	LOG("\tGET %s\n", path);

	//Attach \"index.html\" if they specified a directory
	//as opposed to a file (i.e. favicon.ico)
	if (strchr(path, '.') == NULL) {
		if (path[strlen(path)-1] == '/') {
			strncat(path, "index.php", 11);
		} else {
			strncat(path, "/index.php", 11);
		}

	} else if (strstr(path, "..") != NULL) {
		return send_error(fd, 403, req_info);
	}

	//Check if resource exists
	if (access(path, F_OK) != 0 && strstr(path + strlen(root_site), "/index.php")) {	
		strstr(path, ".php")[0] = '\0';
		strncat(path, ".html", 6);
	}
	
	if (access(path, F_OK) != 0 && strstr(path + strlen(root_site), "/index.html")) {	
		strstr(path, "index.html")[0] = '\0';
		return send_list(fd, path, req_info);
	}

	if (access(path, F_OK) != 0) {
		return send_error(fd, 404, req_info);
	}

	//Check file size
	struct stat file_stat;
	if (stat(path, &file_stat) == -1) {
		perror("stat");
		return 3;
	}
	size_t file_size = (size_t)file_stat.st_size;

	LOG("Final file path: %s, File size: %zu\n", path, file_size);

	if (req_info->range_end == 0) {
		req_info->range_end = file_size;
	}

	//range_end = max(range_end, file_size)
	req_info->range_end = req_info->range_end <= file_size ? req_info->range_end : file_size;
	LOG("Range: bytes=%zu-%zu\n", req_info->range_start, req_info->range_end);

	set_mime_type(path, req_info);

	if (req_info->stage == 1) {

		//send response header, returning on block or error
		int ret = 0;
		if ((ret = send_status_n(fd, 200, req_info, 
				req_info->range_end - req_info->range_start)) != 1) {
			return ret;
		}
		req_info->stage += 1;
		req_info->progress = 0;
	}

	//Get the actual file path, file size, and then write as much as possible
	if (req_info->stage == 2) {

		//Do not send body if this is HEAD and not GET
		if (req_info->req_type == HEAD) {
			return 1;
		}

		FILE *file = fopen(path, "r");

		ssize_t write_status = write_all_to_socket_from_file(fd, file, 
				req_info->range_end - req_info->range_start - req_info->progress, 
				req_info->range_start + req_info->progress);

		while (file != NULL) {
			//Did we make progress?
			if (write_status > 0) {
				req_info->progress += write_status;
			}

			LOG("File GET progress: %zu\n", req_info->progress);

			//Return on block/error, otherwise go to next stage
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				LOG("GET blocked!\n");
				fclose(file);

				//Resume request later
				return 0;
			} else if (errno == SIGPIPE) {
				LOG("Sigpipe on %d\n", fd);
				fclose(file);

				//Ignore request
				return 3;
			} else if (errno != 0) { //SIGPIPE or error
				LOG("Error GETTING file\n");
				fclose(file);

				//Ignore request
				return 3;
			} else if (req_info->progress == file_size) {
				LOG("Completed GETTING file!\n");
				fclose(file);
				return 1; //Success!
			}
		}

		return req_info->progress == file_size;
	}
	return 0;
	
}

int put(request_info *req_info) {

	int fd = req_info->event->data.fd;

	// path = root_site .. path (index.html if needed)
	char path[MAX_PATHNAME_SIZE + strlen(root_site) + 1];
	memcpy(path, root_site, strlen(root_site));

	//Scan path
	if (sscanf(req_info->request_h, "%*s %s", path + strlen(root_site) - 1) != 1) {
		return send_error(fd, 400, req_info);
	}

	LOG("\tPOST %s\n", path);

	//Attach \"index.html\" if they specified a directory
	//as opposed to a file (i.e. favicon.ico)
	if (strchr(path, '.') == NULL) {
		if (path[strlen(path)-1] == '/') {
			strncat(path, "index.html", 12);
		} else {
			strncat(path, "/index.html", 12);
		}
	} else if (strstr(path, "..") != NULL) {
		return send_error(fd, 403, req_info);
	}
 
	if (access(path, F_OK) == 0) {
		remove(path);
	}

	//Check file size
	char *cont_length = strstr(req_info->request_h, "Content-Length:");
	if (cont_length == NULL) {
		LOG("POST but no Content-Length specified\n");
		return 3;
	}

	//Scan in file size
	size_t file_size = 0;
	sscanf(cont_length, "%*s %zu\n", &file_size);

	LOG("Final file path: %s, File size: %zu\n", path, file_size);

	if (req_info->stage == 1) {

		//send response header, returning on block or error
		int ret = 0;
		if ((ret = send_status_n(fd, 200, req_info, file_size)) != 1) { 
			return ret;
		}
		req_info->stage += 1;
		req_info->progress = 0;
	}

	//Get the actual file path, file size, and then write as much as possible
	if (req_info->stage == 2) {

		FILE *file = fopen(path, "w");

		ssize_t write_status = read_all_from_socket_to_file(fd, file, 
			file_size - req_info->progress, req_info->progress);

		fclose(file);

		//Did we make progress?
		if (write_status > 0) {
			req_info->progress += write_status;
		}

		LOG("File POST progress: %zu\n", req_info->progress);

		//Return on block/error, otherwise go to next stage
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			LOG("POST Blocked!\n");
			//Resume request later
			return 0;
		} else if (errno == SIGPIPE) {
			LOG("Sigpipe on %d\n", fd);
			//Ignore request
			return 3;
		} else if (errno != 0) { //SIGPIPE or error
			LOG("Error POSTING file\n");
			//Ignore request
			return 3;
		} else {
			LOG("Completed POSTING file!\n");
			return 1; //Success!
		}
		return req_info->progress == file_size;

	}
}

int send_status(int fd, int status, struct request_info *req_info) {
	LOG("Preparing a status of %d\n", status);

	//Allocate space for the response header
	if (req_info->response_h == NULL) {
		req_info->response_h = calloc(1, MAX_HEADER_SIZE);
	}

	//in case of block and resume, dont overwrite response_h
	if (strlen(req_info->response_h) < 1) {
		char date[100];
		time_t now = time(0);
		struct tm tm = *gmtime(&now);
		strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %Z", &tm);

		sprintf(req_info->response_h, "HTTP/1.1 %d %s\n"
				"Date: %s\n"
				"Connection: close\n",
				status, status_desc[status], date);

		if (req_info->range_end != 0) {
			sprintf(req_info->response_h + strlen(req_info->response_h), 
					"Content-Range: bytes=%zu-%zu\n", req_info->range_start,
					req_info->range_end);
		}

		if (req_info->mime_type != NULL) { //TODO: Make this work
			sprintf(req_info->response_h + strlen(req_info->response_h), 
					"Content-Type: %s\n", req_info->mime_type);
		}	

		strncat(req_info->response_h, security_headers, strlen(security_headers));
	}


	ssize_t write_status = write_all_to_socket(fd, 
			req_info->response_h + req_info->progress, 
			strlen(req_info->response_h) - req_info->progress);

	LOG("\n\tWrite status: %zu\nResponse:\n\"%s\"\n", write_status, req_info->response_h);

	//Did we make progress?
	if (write_status > 0) {
		req_info->progress += write_status;
	}

	//Return on block/error, otherwise go to next stage
	if (errno == EWOULDBLOCK || errno == EAGAIN) {
		LOG("Write blocked!\n");
		//Resume request later
		return 0;
	} else if (errno == SIGPIPE) {
		LOG("Sigpipe on %d\n", fd);
		//Ignore request
		return 3;
	} else if (errno != 0) { //SIGPIPE or error
		LOG("Error writing header\n");
		//Ignore request
		return 3;
	}

	LOG("Completed writing response!\n");
        return 1;
}			

int send_status_n(int fd, int status, struct request_info *req_info, size_t file_size) {
	LOG("Sending a status of %d\n", status);

	//Allocate space for the response header
	if (req_info->response_h == NULL) {
		req_info->response_h = calloc(1, MAX_HEADER_SIZE);
	}

	//in case of block and resume, dont overwrite response_h
	if (strlen(req_info->response_h) < 1) {
		char date[100];
		time_t now = time(0);
		struct tm tm = *gmtime(&now);
		strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %Z", &tm);

		sprintf(req_info->response_h, "HTTP/1.1 %d %s\n"
				"Date: %s\n"
				"Connection: close\n"
				"Content-Length: %zu\n",
				status, status_desc[status], date, file_size);

		if (req_info->range_end != 0) {
			sprintf(req_info->response_h + strlen(req_info->response_h), 
					"Content-Range: bytes=%zu-%zu\n", req_info->range_start,
					req_info->range_end);
		}
		

		if (req_info->mime_type != NULL) { //TODO: Make this work
			sprintf(req_info->response_h + strlen(req_info->response_h), 
					"Content-Type: %s\n", req_info->mime_type);
		}	

		strncat(req_info->response_h, security_headers, strlen(security_headers));
	}

	ssize_t write_status = write_all_to_socket(fd, 
			req_info->response_h + req_info->progress, 
			strlen(req_info->response_h) - req_info->progress);

	LOG("\n\tWrite status: %zu\nResponse:\n\"%s\"\n", write_status, req_info->response_h);

	//Did we make progress?
	if (write_status > 0) {
		req_info->progress += write_status;
	}

	//Return on block/error, otherwise go to next stage
	if (errno == EWOULDBLOCK || errno == EAGAIN) {
		LOG("Write blocked!\n");
		//Resume request later
		return 0;
	} else if (errno == SIGPIPE) {
		LOG("Sigpipe on %d\n", fd);
		//Ignore request
		return 3;
	} else if (errno != 0) { //SIGPIPE or error
		LOG("Error writing header\n");
		//Ignore request
		return 3;
	}

	LOG("Completed writing response!\n");
	
	return 1;
}			

int send_list(int fd, char *path, struct request_info *req_info) {

	//make list in html
	char buff[8096];
	buff[0] = '\0';

        DIR *d = opendir(path);
        struct dirent *dir;

        if (d == NULL) {
            return send_error(fd, 404, req_info);
	} else {			
	    strcat((char*)&buff, HTML_HEADER);
            while ((dir = readdir(d)) != NULL) {
	        if (dir->d_name[0] != '.' && dir->d_name[0] != '-') {
	            sprintf((char*)&buff + strlen((char*)&buff), "<a href=\"%s%s\">%s</a></br>", path + strlen(root_site), dir->d_name, dir->d_name);
	        }
	    }

            closedir(d);	
	    strcat((char*)&buff, HTML_FOOTER);
        }    	

    	int file_size = strlen((char*)&buff);
	LOG("Sending directory listing to %d for %s\n", fd, path);

	if (req_info->stage == 1) {
		int ret;
		if ((ret = send_status_n(fd, 200, req_info, file_size)) != 1) {
	        	return ret;
		}
		req_info->stage += 1;
		req_info->progress = 0;
	}

	//Get the actual file path, file size, and then write as much as possible
	if (req_info->stage == 2) {
		ssize_t write_status = write_all_to_socket(fd, 
				(char*)&buff + req_info->progress, 
				strlen((char*)&buff - req_info->progress));

		//Did we make progress?
		if (write_status > 0) {
			req_info->progress += write_status;
		}

		//Return on block/error, otherwise go to next stage
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			LOG("Write blocked!\n");
			//Resume request later
			return 0;
		} else if (errno == SIGPIPE) {
			LOG("Sigpipe on %d\n", fd);
			//Ignore request
			return 3;
		} else if (errno != 0) { //SIGPIPE or error
			LOG("Error writing header\n");
			//Ignore request
			return 3;
		}
		return req_info->progress == strlen((char*)&buff);
	}

}			
int send_error(int fd, int status, struct request_info *req_info) {

	//make list in html
	char buff[8096];
	buff[0] = '\0';

	strcat((char*)&buff, HTML_HEADER);
	sprintf((char*)&buff + strlen((char*)&buff), "<h2>Error: %d %s</h2>", status, status_desc[status]);
	strcat((char*)&buff, HTML_FOOTER);    	

    	int file_size = strlen((char*)&buff);
	LOG("Sending Error page %d to %d\n", status, fd);

	if (req_info->stage == 1) {
		int ret;
		if ((ret = send_status_n(fd, 200, req_info, file_size)) != 1) {
	        	return ret;
		}
		req_info->stage += 1;
		req_info->progress = 0;
	}

	//Get the actual file path, file size, and then write as much as possible
	if (req_info->stage == 2) {
		ssize_t write_status = write_all_to_socket(fd, 
				(char*)&buff + req_info->progress, 
				strlen((char*)&buff - req_info->progress));

		//Did we make progress?
		if (write_status > 0) {
			req_info->progress += write_status;
		}

		//Return on block/error, otherwise go to next stage
		if (errno == EWOULDBLOCK || errno == EAGAIN) {
			LOG("Write blocked!\n");
			//Resume request later
			return 0;
		} else if (errno == SIGPIPE) {
			LOG("Sigpipe on %d\n", fd);
			//Ignore request
			return 3;
		} else if (errno != 0) { //SIGPIPE or error
			LOG("Error writing header\n");
			//Ignore request
			return 3;
		}
		return req_info->progress == strlen((char*)&buff);
	}

}	
void set_mime_type(char *path, struct request_info *req_info) {
	//check mime type
	if (strstr(path, ".html") != NULL) {
		req_info->mime_type = "text/html";
	} else if (strstr(path, ".css") != NULL) {
		req_info->mime_type = "text/css";
	} else if (strstr(path, ".js") != NULL) {
		req_info->mime_type = "text/javascript";
	} else if (strstr(path, ".mp4") != NULL) {
		req_info->mime_type = "video/mp4";
	} else if (strstr(path, ".jpg") != NULL) {
		req_info->mime_type = "image/jpeg";
	} else if (strstr(path, ".png") != NULL) {
		req_info->mime_type = "image/png";
	} else {
		req_info->mime_type = magic_file(magic, path);
	}
}

void parse_config(config_t* cf) {

	const char *log_file_path = NULL;

	if (config_read_file(cf, CONFIG_FILE)) {

		//root path
		const char* temp_root = NULL;
		config_lookup_string(cf, "webserver_root", &temp_root);
		root_site = strdup(temp_root);
		LOG("Root of webserver: %s\n", root_site);

		//port
		const char* temp_port = NULL;
		config_lookup_string(cf, "port", &temp_port);
		port = strdup(temp_port);
		LOG("Host port: %s\n", port);

		//log
		config_lookup_string(cf, "log_file", &log_file_path);
		if (log_file_path != NULL) {

			http_log = fopen(log_file_path, "a");
			if (http_log == NULL) {
				perror("Couldn't find log file");
				config_destroy(cf);
				graceful_exit(0);
			}

			LOG("Using log file at %s\n", log_file_path);
		}

		const config_setting_t* s_headers = config_lookup(cf, "security_headers");
		if (s_headers == NULL) {
			security_headers = DEFAULT_SECURITY_HEADERS;
		} else {
			security_headers = malloc(256);
			security_headers[0] = '\0';
			for (int i = 0; i < config_setting_length(s_headers); i++) {
				const char* header = config_setting_get_string_elem(s_headers, i);

				char buf[256];
				sprintf((char*)&buf, "%s\n", header);
				strcat(security_headers, (char*)&buf);
			}

			strcat(security_headers, "\n");

			LOG("Security headers:\n\n%s", security_headers);
		}

		config_lookup_int(cf, "max_file_size", &max_file_size);
		LOG("Using max file size of %d\n", max_file_size);

		config_lookup_int(cf, "timeout_ms", &timeout_ms);
		if (timeout_ms <= 0) {
			timeout_ms = DEFAULT_TIMEOUT_MS;
		}

		LOG("Using timeout of %d\n", timeout_ms);

	} else {
		perror("Couldnt get config file");
		config_destroy(cf);
		graceful_exit(0);
	}

	config_destroy(cf);
	if (port == NULL || root_site == NULL) {
		print_usage();
	}
}
