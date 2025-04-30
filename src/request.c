// Holden Caldwell
// I want to be very clear, this was AI assisted. Not AI created, I certainly did not copy and paste, but I did use AI to help me understand what needed to be done, and some of the finer aspects of the C code that I am unfamilar with
// credit: Perplexity.ai

#include "io_helper.h"
#include "request.h"
#include <pthread.h>


#define MAXBUF (8192)

int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;

// start by initializing the task structure
typedef struct {
    int fd; //client socket?
    struct stat file_info; //File Information
    char filename[MAXBUF]; //path
} RequestTask;

// Init Buffer Structure
typedef struct {
    RequestTask *buffer; // stores in an aray using RequestTask struct
    int buffer_cap; // max size of the buffer
    int front;
    int rear; 
    int count;

    //define locks inside of the structure
    pthread_mutex_t lock; //buffer lock
    pthread_cond_t not_empty; //lets us know that the buffer is NOT empty
    pthread_cond_t not_full; // Lets us know when we have space to add to the buffer
    //two conditional variables because we have a producer consumer problem!
} RequestBuffer;

static RequestBuffer request_buffer; // global buffer variable




// next we need to intialize the actual buffer
void buffer_init (RequestBuffer *rbuf, int size) { //making a buffer called rbuf... will use that for the rest of the function
  rbuf -> buffer_cap = size; // set the buffer size to the size given
  rbuf -> buffer = malloc(size * sizeof(RequestTask)); // allocate memory for the tasks
  if (rbuf->buffer == NULL) {
    perror("Memory not allocated for the request buffer");
    exit(1);
  }
  rbuf->front=0;
  rbuf->rear =0;
  rbuf->count=0;

  pthread_mutex_init(&rbuf->lock, NULL);
  pthread_cond_init(&rbuf->not_empty, NULL);
  pthread_cond_init(&rbuf->not_full, NULL);
}

void buffer_add (RequestBuffer *rbuf, RequestTask *task) { // add a task to the buffer
  pthread_mutex_lock(&rbuf->lock); // lock the buffer
  while (rbuf->count == rbuf->buffer_cap) { // if the buffer is full
    pthread_cond_wait(&rbuf->not_full, &rbuf->lock); // wait for space to be available
  }
  memcpy(&rbuf->buffer[rbuf->rear], task, sizeof(RequestTask)); // add the task to the buffer
  rbuf->rear = (rbuf->rear + 1) % rbuf->buffer_cap; // move the rear pointer
  rbuf->count++; // increment the count
  pthread_cond_signal(&rbuf->not_empty); // signal that the buffer is not empty
  pthread_mutex_unlock(&rbuf->lock); // unlock the buffer
}

void buffer_destroy (RequestBuffer *rbuf) { // destroy the buffer
  free(rbuf->buffer); // free the memory
  pthread_mutex_destroy(&rbuf->lock); // destroy the lock
  pthread_cond_destroy(&rbuf->not_empty); // destroy the condition variable
  pthread_cond_destroy(&rbuf->not_full); // destroy the condition variable
}



//
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXBUF], body[MAXBUF];
    
    // Create the body of error message first (have to know its length for header)
    sprintf(body, ""
	    "<!doctype html>\r\n"
	    "<head>\r\n"
	    "  <title>CYB-3053 WebServer Error</title>\r\n"
	    "</head>\r\n"
	    "<body>\r\n"
	    "  <h2>%s: %s</h2>\r\n" 
	    "  <p>%s: %s</p>\r\n"
	    "</body>\r\n"
	    "</html>\r\n", errnum, shortmsg, longmsg, cause);
    
    // Write out the header information for this response
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Type: text/html\r\n");
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
    write_or_die(fd, buf, strlen(buf));
    
    // Write out the body last
    write_or_die(fd, body, strlen(body));
    
    // close the socket connection
    close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd) {
    char buf[MAXBUF];
    
    readline_or_die(fd, buf, MAXBUF);
    while (strcmp(buf, "\r\n")) {
		readline_or_die(fd, buf, MAXBUF);
    }
    return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
//
int request_parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;
    
    if (!strstr(uri, "cgi")) { 
	// static
	strcpy(cgiargs, "");
	sprintf(filename, ".%s", uri);
	if (uri[strlen(uri)-1] == '/') {
	    strcat(filename, "index.html");
	}
	return 1;
    } else { 
	// dynamic
	ptr = index(uri, '?');
	if (ptr) {
	    strcpy(cgiargs, ptr+1);
	    *ptr = '\0';
	} else {
	    strcpy(cgiargs, "");
	}
	sprintf(filename, ".%s", uri);
	return 0;
    }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) 
		strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) 
		strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) 
		strcpy(filetype, "image/jpeg");
    else 
		strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
void request_serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXBUF], buf[MAXBUF];
    
    request_get_filetype(filename, filetype);
    srcfd = open_or_die(filename, O_RDONLY, 0);
    
    // Rather than call read() to read the file into memory, 
    // which would require that we allocate a buffer, we memory-map the file
    srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close_or_die(srcfd);
    
    // put together response
    sprintf(buf, ""
	    "HTTP/1.0 200 OK\r\n"
	    "Server: OSTEP WebServer\r\n"
	    "Content-Length: %d\r\n"
	    "Content-Type: %s\r\n\r\n", 
	    filesize, filetype);
       
    write_or_die(fd, buf, strlen(buf));
    
    //  Writes out to the client socket the memory-mapped file 
    write_or_die(fd, srcp, filesize);
    munmap_or_die(srcp, filesize);
}

static void init_once_wrapper(){
  buffer_init(&request_buffer, buffer_max_size); // initialize the buffer
}
// this function is called once to initialize the buffer... apparently the wrapper is important for some reason?

void FIFO (RequestBuffer *rbuf, RequestTask *task) { // remove a task from the buffer
  printf("FIFO\n");
  pthread_mutex_lock(&rbuf->lock); // lock the buffer
  while (rbuf->count == 0) { // if the buffer is empty
    pthread_cond_wait(&rbuf->not_empty, &rbuf->lock); // wait for a task to be available
  }
  *task = rbuf->buffer[rbuf->front]; // get the task from the buffer
  rbuf->front = (rbuf->front + 1) % rbuf->buffer_cap; // move the front pointer
  rbuf->count--; // decrement the count
  pthread_cond_signal(&rbuf->not_full); // signal that the buffer is not full
  pthread_mutex_unlock(&rbuf->lock); // unlock the buffer
} 

void SFF (RequestBuffer *rbuf, RequestTask *task) { // remove a task from the buffer
  printf("SFF\n");
  pthread_mutex_lock(&rbuf->lock); // lock the buffer
  while (rbuf->count == 0) { // if the buffer is empty
    pthread_cond_wait(&rbuf->not_empty, &rbuf->lock); // wait for a task to be available
  }
  int min_index = rbuf ->front; // index of the task with the smallest size
  for (int i = 0; i < rbuf->count; i++) { // loop through the buffer
    if (rbuf->buffer[i].file_info.st_size < rbuf->buffer[min_index].file_info.st_size) { // if the task is smaller
      min_index = i; // update the index
    }
  }
  if (min_index != rbuf->front) { // if the task is not at the front
    RequestTask temp = rbuf->buffer[rbuf->front]; // swap the tasks so we can do a normal remove
    rbuf->buffer[rbuf->front] = rbuf->buffer[min_index];
    rbuf->buffer[min_index] = temp;
  }
  *task = rbuf->buffer[rbuf->front]; // get the task from the buffer
  rbuf->front = (rbuf->front + 1) % rbuf->buffer_cap; // move the front pointer
  rbuf->count--; // decrement the count
  pthread_cond_signal(&rbuf->not_full); // signal that the buffer is not full
  pthread_mutex_unlock(&rbuf->lock); // unlock the buffer
}
void RANDOM (RequestBuffer *rbuf, RequestTask *task) { // remove a task from the buffer
  printf("RANDOM\n");
  pthread_mutex_lock(&rbuf->lock); // lock the buffer
  while (rbuf->count == 0) { // if the buffer is empty
    pthread_cond_wait(&rbuf->not_empty, &rbuf->lock); // wait for a task to be available
  }
  int random_index = rand() % rbuf->count; // get a random index
  *task = rbuf->buffer[random_index]; // get the task from the buffer
  rbuf->buffer[random_index] = rbuf->buffer[rbuf->front]; // swap the tasks so we can do a normal remove
  rbuf->front = (rbuf->front + 1) % rbuf->buffer_cap; // move the front pointer
  rbuf->count--; // decrement the count
  pthread_cond_signal(&rbuf->not_full); // signal that the buffer is not full
  pthread_mutex_unlock(&rbuf->lock); // unlock the buffer
}


void* thread_request_serve_static(void* arg)
{
  
  static pthread_once_t buffer_init = PTHREAD_ONCE_INIT; // this is a static variable only intialized once
	pthread_once(&buffer_init, init_once_wrapper); // initialize the buffer once
  while (1) { // loop until the buffer is empty
    RequestTask task;
    if (scheduling_algo == 0) {
      FIFO(&request_buffer, &task); // get the task from the buffer
    } else if (scheduling_algo == 1) {
      SFF(&request_buffer, &task); // get the task from the buffer
    } else if (scheduling_algo == 2) {
      RANDOM(&request_buffer, &task); // get the task from the buffer
    }
    request_serve_static(task.fd, task.filename, task.file_info.st_size); // serve the static content
     // close the socket connection
    close_or_die(task.fd);
  }
 


}

//
// Initial handling of the request
//
void request_handle(int fd) {
    printf("Handling request\n");
    int is_static;
    struct stat sbuf;
    char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    char filename[MAXBUF], cgiargs[MAXBUF];
    
	// get the request type, file path and HTTP version
    readline_or_die(fd, buf, MAXBUF);
    sscanf(buf, "%s %s %s", method, uri, version);
    
    

	// verify if the request type is GET or not
    if (strcasecmp(method, "GET")) {
		request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
		return;
    }
    request_read_headers(fd);
    
	// check requested content type (static/dynamic)
    is_static = request_parse_uri(uri, filename, cgiargs);
    
	// get some data regarding the requested file, also check if requested file is present on server
    if (stat(filename, &sbuf) < 0) {
		request_error(fd, filename, "404", "Not found", "server could not find this file");
		return;
    }
    
	// verify if requested content is static
    if (is_static) {
		if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
			request_error(fd, filename, "403", "Forbidden", "server could not read this file");
			return;
		}

    // TODO: Add can't escape from current directory check - IF statement
    if (strstr(filename, "../")) {
      request_error(fd, filename, "403", "Forbidden", "server could not read this file"); 
      return;
    }   
      
    
    // add the task to the buffer
    printf("Adding task to buffer\n");
    RequestTask new_task;
    new_task.fd = fd; // set the file descriptor
    new_task.file_info = sbuf; // set the file info
    strcpy(new_task.filename, filename); // set the filename
    buffer_add(&request_buffer, &new_task); // add the task to the buffer

    } else {
		request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
    }
}
