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

/* HTTP Response Headers */
#define HTTP_OK "HTTP/1.1 200 OK\r\n"\
                "Server: Single File Server\r\n"\
                "Content-Type: text/html; charset=iso-8859-1\r\n"\
                "Connection: Closed\r\n\r\n"

#define HTTP_404 "HTTP/1.1 404 Not Found\r\n"\
                "Server: Single File Server\r\n"\
                "Content-Type: text/html; charset=iso-8859-1\r\n"\
                "Connection: Closed\r\n\r\n"\
                "Requested Page Not Found :)\r\n"

#define HELPSTRING "Usage: %s [-p port number] [-h html directory] [-t thread pool size]\n"\
                   "-p\t\tServer port (Default 8000) [OPTIONAL]\n"\
                   "-h\t\tHTML Directory. HTML files in this directory are served."\
                   "This directory must be in the same directory as the server executable and don't add './' to the directory name! (Default 'html') [OPTIONAL]\n"\
                   "-t\t\tThread pool size. Number of threads for the server to use. (Default 8) [OPTIONAL]\n"\
                   "-h\t\tShows available arguments\n"

struct threadpool {
    pthread_t *workers;
    pthread_mutex_t lock;
    pthread_cond_t cond_var;
};
typedef struct threadpool threadpool;

struct qnode {
    int clientfd;
    struct sockaddr_in client_addr;
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
int is_running;

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
int enqueue(queue *q, int clientfd, struct sockaddr_in client_addr)
{
    if(q->size == q->capacity) {
        return -1;
    }
    qnode *node = calloc(1, sizeof(qnode));
    node->clientfd = clientfd;
    node->client_addr = client_addr;
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
    q->size--;
    qnode *retnode = q->head;
    q->head = q->head->next;
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
                printf("%s", HELPSTRING);
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
    return 0;
}

/* Returns the filename extracted from HTTP GET request */
char *get_file_name(char *request)
{
    char method[8] = {'\0'}, version[8] = {'\0'};
    char *filename = calloc(1, 32);
    sscanf(request, "%s %s %s", method, filename, version);
    return filename;
}

/* Thread function that handles a single client in a blocking fashion. This function takes no arguments.
 * The connection queue pointer is global and mutex locks and condition variables are global.
 */
void* handle_connection(void *args)
{
    sleep(1);
    int clientfd, htmlfd;
    struct sockaddr_in client_addr;
    char buffer[REQUEST_BUFFER_SIZE];
    char *fullpath, *filename, *filebuffer;
    qnode *node;
    int br,bw;
    while(is_running) {
        /* Dequeue a connection */
        pthread_mutex_lock(&(pool->lock));
        pthread_cond_wait(&(pool->cond_var), &(pool->lock));
        node = dequeue(connqueue);
        pthread_mutex_unlock(&(pool->lock));
        if(node == NULL)
            continue;

        clientfd = node->clientfd;
        client_addr = node->client_addr;
        free(node);

        br = read(clientfd, buffer, REQUEST_BUFFER_SIZE-1);
        /* check if read() failed */
        if(br <= 0)
            goto close_clientfd;
        buffer[br] = '\0';
        /* Check for end of HTTP request header */
        if(strstr(buffer, "\r\n\r\n") == NULL)
            goto close_clientfd;
        filename = get_file_name(buffer);
        if(strcmp(filename, "/") == 0) {
            sprintf(filename, "/index.html");
        }
        /* attempt to read html file */
        fullpath = calloc(1, strlen(html_dir) + strlen(filename) + 4);
        sprintf(fullpath, "./%s%s", html_dir, filename);
        printf("fullpath: %s\n", fullpath);
        htmlfd = open(fullpath, O_RDONLY);
        free(fullpath);
        free(filename);
        if(htmlfd < 0) {
            bw = write(clientfd, HTTP_404, sizeof(HTTP_404));
            goto close_clientfd;
        } else {
            filebuffer = calloc(1, FILE_BUFFER_SIZE);
            bw = write(clientfd, HTTP_OK, sizeof(HTTP_OK));
            if(bw <= 0)
                goto close_clientfd;
            while((br = read(htmlfd, filebuffer, FILE_BUFFER_SIZE-1))) {
                bw = write(clientfd, filebuffer, br);
                if(bw <= 0)
                    goto close_clientfd;
                memset(filebuffer, 0, bw);
            }
            free(filebuffer);
        }
        close(htmlfd);
close_clientfd:
        close(clientfd);
    }
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
    signal(SIGPIPE, NULL);

    /* Start server loop */
    fd_set readset;
    int clientfd = -1;
    struct sockaddr_in client_addr;
    socklen_t len;
    is_running = 1;
    while(is_running) {
        /* setup for select() */
        FD_ZERO(&readset);
        FD_SET(listenfd, &readset);
        FD_SET(STDIN_FILENO, &readset);
        if (select(listenfd + 1, &readset, NULL, NULL, NULL) < 0) {
            is_running = 0;
            perror("select() error\n");
            break;
        }

        /* handle console input */
        if(FD_ISSET(STDIN_FILENO, &readset)) {
            if(handle_stdin()) {
                is_running = 0;
                break;
            }
        }

        /* handle new connection */
        if(FD_ISSET(listenfd, &readset)) {
            memset(&client_addr, 0, sizeof(client_addr));
            clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &len);
            pthread_mutex_lock(&(pool->lock));
            if(enqueue(connqueue, clientfd, client_addr) < 0){
                printf("Connection capacity reached. Dropped new connection!\n");
                close(clientfd);
                pthread_mutex_unlock(&(pool->lock));
            } else {
                pthread_cond_signal(&(pool->cond_var));
                pthread_mutex_unlock(&(pool->lock));
            }
        }
    }

    /* Clean up */
    freequeue(connqueue);
    free(connqueue);
    printf("stopping server\n");
    return EXIT_SUCCESS;
}
