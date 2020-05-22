#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

const int LISTEN_BACKLOG = 32;
const int MAX_CONNECTIONS = 64;
const int REQUEST_BUFFER_SIZE = 1024;
const int FILE_BUFFER_SIZE = 1024;

const int default_port = 8000;
const int default_poolsize = 8;
#define default_static "html";

/* Help string */
#define HELPSTRING "Static HTTP server\n"\
                   "Usage: %s [-p port number] [-h html directory] [-t thread pool size]\n"\
                   "\n[Optional arguments]\n"\
                   "\t-p\tServer port (Default 8000)\n"\
                   "\t-t\tThread pool size. Number of threads for the server to use. (Default 8)\n"\
                   "\t-s\tHTML files in this directory are served. This directory must be in the same directory\n"\
                   "\t\tas the server binary and don't add './' to the directory name! (Default 'html')\n"\
                   "\t-h\tShows available arguments\n"


/* HTTP Response Headers */
#define HTTP_BASE_OK "HTTP/1.1 200 OK\r\n"\
                "Server: Single File Server\r\n"\
                "Connection: Closed\r\n"\
                "Content-Type: "\

#define HTTP_404 "HTTP/1.1 404 Not Found\r\n"\
                 "Server: Single File Server\r\n"\
                 "Content-Type: text/html; charset=utf-8\r\n"\
                 "Connection: Closed\r\n\r\n"

#define HTTP_405 "HTTP/1.1 405 Method Not Allowed\r\n"\
                 "Server: Single File Server\r\n"\
                 "Content-Type: text/html; charset=utf-8\r\n"\
                 "Connection: Closed\r\n\r\n"

const int HTTP_BASE_OK_len = strlen(HTTP_BASE_OK);
const int HTTP_404_len = strlen(HTTP_404);
const int HTTP_405_len = strlen(HTTP_405);

volatile unsigned int bytes_read = 0;
volatile unsigned int bytes_wrote = 0;

/* Supported filetypes */
enum filetype {
    HTML,
    CSS,
    JPG,
    PNG,
    TXT,
    SVG,
    UNKNOWN
};

struct threadpool {
    pthread_t *workers;
    pthread_mutex_t lock;
    pthread_cond_t cond_var;
};
typedef struct threadpool threadpool;

struct qnode {
    int clientfd;
    struct qnode *next;
};
typedef struct qnode qnode;

struct queue {
    int size;
    int capacity;
    qnode *head;
    qnode *tail;
};
typedef struct queue queue;

/* Global variables for threads */
threadpool *pool;
queue *connqueue;
char *html_dir;

/* Creates a queue of given capacity */
queue *create_queue(int capacity)
{
    queue *q = calloc(1, sizeof(queue));
    q->capacity = capacity;
    q->size = 0;
    q->head = NULL;
    q->tail = NULL;
    return q;
}

/* Enqueues a new connection */
int enqueue(queue *q, int clientfd)
{
    if(q->size == q->capacity) {
        return -1;
    }
    qnode *node = calloc(1, sizeof(qnode));
    node->clientfd = clientfd;
    node->next = NULL;
    if(q->size == 0) {
        q->head = node;
        q->tail = node;
        q->size = 1;
    } else {
        q->tail = node;
        q->size++;
    }
    return 1;
}

/* Dequeues a connection from the queue and returns pointer to the dequeued
 * node. Its upto the caller to free the dequeued node
 */
qnode *dequeue(queue *q)
{
    if(q->size == 0)
        return NULL;
    qnode *retnode = q->head;
    q->head = q->head->next;
    q->size--;
    return retnode;
}

/* Deallocates the memory given to queue */
void freequeue(queue *q)
{
    qnode *cursor = q->head, *temp;
    while(cursor != NULL) {
        temp = cursor;
        cursor = cursor->next;
        free(temp);
    }
    free(q);
    return ;
}

/* Parses command line arguments and returns port, threads, html_dir via pointers
 * (only if they were passed in the first place)
 */
int parse_args(int argc, char *argv[], int *port, int *threads, char **html_dir)
{
    int option;
    while ((option = getopt(argc, argv, "p:t:s:h")) != -1) {
        switch(option) {
            case 'p':
                *port = atoi(optarg);
                break;
            case 't':
                *threads = atoi(optarg);
                break;
            case 's':
                *html_dir = optarg;
                break;
            case 'h':
                printf(HELPSTRING, argv[0]);
                return 0;
            default:
                printf("Invalid usage. Try -h for help\n");
                return 0;
                exit(EXIT_FAILURE);
        }
    }
    return 1;
}

/* Returns if ''html_dir' exists in the current directory */
int check_html_dir()
{
    /* Make sure that root (/) or home (~) is not accessed. */
    if(html_dir[0] == '/' || html_dir[0] == '~') {
        printf("%s is invalid. Make sure that your static directory is present in the current directory\n", html_dir);
        return 0;
    }

    struct stat s;
    int res = stat(html_dir, &s);
    if(res == -1) {
        if(errno == ENOENT) {
            printf("%s does not exist\n", html_dir);
        } else {
            perror("stat error\n");
        }
    } else {
        if (S_ISDIR(s.st_mode)) {
            return 1;
        } else {
            printf("%s is not a directory\n", html_dir);
        }
    }
    return 0;
}

/* Creates a listen socket on supplied port */
int create_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        perror("Failed to create listen socket\n");
        return -1;
    }
    /* Make socket reusable */
    int enable = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        return -1;
    }
    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(port);
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        perror("Failed to bind listen socket\n");
        return -1;
    }
    if(listen(fd, LISTEN_BACKLOG) < 0) {
        perror("listen() error on socket\n");
        return -1;
    }
    return fd;
}

/* Returns 1 is the user entered exit or quit */
int handle_stdin()
{
    char input[8];
    fgets(input, 7, stdin);
    input[7] = '\0';
    if(!strcmp(input, "exit\n") || !strcmp(input, "quit\n"))
        return 1;
    printf("Enter 'exit' or 'quit' (without quotes) to stop the server\n");
    return 0;
}

/* Returns the filename extracted from HTTP GET request */
char *get_file_name(char *request)
{
    char method[8] = {'\0'}, version[8] = {'\0'};
    char *filename = calloc(1, 32);
    sscanf(request, "%s %s %s\r\n", method, filename, version);
    /* If method is GET, return NULL */
    if (strcmp(method, "GET") != 0)
        return NULL;
    return filename;
}

/* Returns filetype based on file extension */
enum filetype get_filetype(char *filename)
{
    char *ext = strchr(filename, '.');
    ext = strchr(filename, '.');
    ext++;
    if(strcmp(ext, "html") == 0)
        return HTML;
    else if(strcmp(ext, "css") == 0)
        return CSS;
    else if(strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0)
        return JPG;
    else if(strcmp(ext, "png") == 0)
        return PNG;
    else if(strcmp(ext, "svg") == 0)
        return SVG;
    else if (strcmp(ext, "txt") == 0)
        return TXT;
    else
        return UNKNOWN;
}

/* Creates the HTTP response HEADER based on file extension */
char *get_response_header(char *filename)
{
    enum filetype ft = get_filetype(filename);
    char *response_header = calloc(1, HTTP_BASE_OK_len + 32);
    switch(ft) {
        case HTML:
            sprintf(response_header, "%stext/html; charset=utf-8\r\n\r\n", HTTP_BASE_OK);
            break;
        case CSS:
            sprintf(response_header, "%stext/css; charset=utf-8\r\n\r\n", HTTP_BASE_OK);
            break;
        case TXT:
            sprintf(response_header, "%stext/plain; charset=utf-8\r\n\r\n", HTTP_BASE_OK);
            break;
        case JPG:
            sprintf(response_header, "%simage/jpeg\r\n\r\n", HTTP_BASE_OK);
            break;
        case PNG:
            sprintf(response_header, "%simage/png\r\n\r\n", HTTP_BASE_OK);
            break;
        case SVG:
            sprintf(response_header, "%simage/svg+xml\r\n\r\n", HTTP_BASE_OK);
            break;
        default:
            sprintf(response_header, "%sapplication/octet-stream\r\n\r\n", HTTP_BASE_OK);
            break;
    }
    return response_header;
}

/* Thread function that handles a single client in a blocking fashion. This function takes no arguments.
 * The connection queue pointer is global and mutex locks and condition variables are global.
 */
void* handle_connection(void *args)
{
    sleep(1);
    int clientfd, htmlfd;
    char buffer[REQUEST_BUFFER_SIZE];
    char *fullpath, *filename, *response_header;
    char *filebuffer = calloc(1, FILE_BUFFER_SIZE);
    qnode *node;
    int br,bw;
    while(1) {
        /* Dequeue a connection */
        pthread_mutex_lock(&(pool->lock));
        pthread_cond_wait(&(pool->cond_var), &(pool->lock));
        node = dequeue(connqueue);
        pthread_mutex_unlock(&(pool->lock));
        /* Node will never be NULL, but for the safety */
        if(node == NULL)
            continue;
        clientfd = node->clientfd;
        free(node);
        /* read request */
        br = read(clientfd, buffer, REQUEST_BUFFER_SIZE-1);
        /* check if read() failed */
        if(br <= 0)
            goto close_clientfd;
        bytes_read += br;
        buffer[br] = '\0';
        /* Check for end of HTTP request header */
        if(strstr(buffer, "\r\n\r\n") == NULL)
            goto close_clientfd;
        filename = get_file_name(buffer);
        /* Method is not GET */
        if(filename == NULL) {
            bw = write(clientfd, HTTP_405, HTTP_405_len);
            if(bw > 0)
                bytes_wrote += bw;
            goto close_clientfd;
        }
        /* If path is '/' server index.html, works only for root of html directory */
        if(strcmp(filename, "/") == 0) {
            sprintf(filename, "/index.html");
        }
        /* attempt to read html file */
        fullpath = calloc(1, strlen(html_dir) + strlen(filename) + 4);
        sprintf(fullpath, "./%s%s", html_dir, filename);
        htmlfd = open(fullpath, O_RDONLY);
        if(htmlfd < 0) {
            bw = write(clientfd, HTTP_404, HTTP_404_len);
            if(bw > 0)
                bytes_wrote += bw;
            goto close_clientfd;
        } else {
            response_header = get_response_header(filename);
            bw = write(clientfd, response_header, strlen(response_header));
            if(bw <= 0)
                goto close_clientfd;
            bytes_wrote += bw;
            while((br = read(htmlfd, filebuffer, FILE_BUFFER_SIZE-1))) {
                bytes_read += br;
                filebuffer[br] = '\0';
                bw = write(clientfd, filebuffer, br);
                if(bw <= 0)
                    goto close_clientfd;
                bytes_wrote += bw;
                memset(filebuffer, 0, bw);
            }
            close(htmlfd);
        }
close_clientfd:
        free(fullpath);
        free(filename);
        close(clientfd);
    }
    free(filebuffer);
    return NULL;
}

/* Creates a threadpool with 'poolsize' number of threads each running
 * the thread_func() function
 */
threadpool* create_threadpool(int poolsize, void *thread_func)
{
    threadpool* pool = malloc(sizeof(threadpool));
    if (pthread_mutex_init(&(pool->lock), NULL) != 0) {
        perror("Error Initialising mutex lock\n");
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&(pool->cond_var), NULL) != 0){
        pthread_mutex_destroy(&(pool->lock));
        perror("Error Initialising conditional variable\n");
        free(pool);
        return NULL;
    }
    pool->workers = calloc(poolsize, sizeof(pthread_t));
    int i;
    for(i = 0; i < poolsize; i++) {
        if(pthread_create(&(pool->workers[i]), NULL, thread_func, NULL) < 0){
            perror("Error in pthread_create()\n");
            pthread_mutex_destroy(&(pool->lock));
            pthread_cond_destroy(&(pool->cond_var));
            free(pool->workers);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

int main(int argc, char *argv[])
{
    int port = default_port;
    int threads = default_poolsize;
    html_dir = default_static;

    /* Parse arguments */
    if(!parse_args(argc, argv, &port, &threads, &html_dir))
        exit(EXIT_FAILURE);

    /* check static directory */
    if(!check_html_dir())
        exit(EXIT_FAILURE);

    /* open listen socket */
    int listenfd;
    if((listenfd = create_listen_socket(port)) < 0)
            exit(EXIT_FAILURE);

    /* Create connection queue */
    connqueue = create_queue(MAX_CONNECTIONS);

    /* initiate thread pool */
    if((pool = create_threadpool(threads, handle_connection)) == NULL)
        exit(EXIT_FAILURE);

    /* Print server configuration */
    printf("Server running on port: %d\nthreads: %d\nstatic directory: %s\nType exit or quit to exit\n", port, threads, html_dir);

    /* ignore broken pipe */
    signal(SIGPIPE, SIG_IGN);

    /* Start server loop */
    fd_set readset;
    int clientfd = -1;
    struct sockaddr_in client_addr;
    socklen_t len;
    while(1) {
        /* setup for select() */
        FD_ZERO(&readset);
        FD_SET(listenfd, &readset);
        FD_SET(STDIN_FILENO, &readset);
        if (select(listenfd + 1, &readset, NULL, NULL, NULL) < 0) {
            perror("select() error\n");
            break;
        }

        /* handle console input */
        if(FD_ISSET(STDIN_FILENO, &readset)) {
            if(handle_stdin()) {
                break;
            }
        }

        /* handle new connection */
        if(FD_ISSET(listenfd, &readset)) {
            memset(&client_addr, 0, sizeof(client_addr));
            len = sizeof(client_addr);
            clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &len);
            pthread_mutex_lock(&(pool->lock));
            if(enqueue(connqueue, clientfd) < 0){
                printf("Connection capacity reached. Dropped new connection!\n");
                close(clientfd);
            } else {
                pthread_cond_signal(&(pool->cond_var));
            }
            pthread_mutex_unlock(&(pool->lock));
        }
    }

    /* Clean up */
    freequeue(connqueue);
    printf("stopping server\n");
    int i;
    for(i = 0; i < threads; i++) {
        pthread_cancel(pool->workers[i]);
    }

    /* Prints stats */
    printf("Total bytes received: %u Bytes\nTotal bytes sent: %u Bytes\n", bytes_read, bytes_wrote);

    return EXIT_SUCCESS;
}
