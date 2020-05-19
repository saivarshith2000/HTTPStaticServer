#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>

const int LISTEN_BACKLOG = 32;
const int MAX_EVENTS = 128;
const int REQUEST_BUFFER_SIZE = 1024;
const int FILE_BUFFER_SIZE = 1024;

const int default_port = 8000;
const int default_poolsize = 32;
#define default_static "./public/";

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

/* Parses command line arguments and returns port, threads, static_dir via pointers
 * (only if they were passed in the first place)
 */
int parse_args(int argc, char *argv[], int *port, int *threads, char **static_dir)
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
                *static_dir = optarg;
                break;
            case 'h':
                printf("Usage: %s [-p port number] [-s static directory] [-t thread pool size]\n", argv[0]);
                return 0;
            default:
                printf("Invalid usage. Try -h for help\n");
                return 0;
                exit(EXIT_FAILURE);
        }
    }
    return 1;
}

/* Returns if ''static_dir' exists in the current directory */
int check_static_dir(char *static_dir)
{
    /* Make sure that root (/) or home (~) is not accessed. */
    if(static_dir[0] == '/' || static_dir[0] == '~') {
        printf("%s is invalid. Make sure that your static directory is present in the current directory\n", static_dir);
        return 0;
    }

    struct stat s;
    int res = stat(static_dir, &s);
    if(res == -1) {
        if(errno == ENOENT) {
            printf("%s does not exist\n", static_dir);
        } else {
            perror("stat error\n");
        }
    } else {
        if (S_ISDIR(s.st_mode)) {
            return 1;
        } else {
            printf("%s is not a directory\n", static_dir);
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

/* Temporary function to handle incoming connection */
void handle_connection(int clientfd, struct sockaddr_in client_addr, char* static_dir)
{
    char buffer[REQUEST_BUFFER_SIZE];
    int br,bw;
    br = read(clientfd, buffer, REQUEST_BUFFER_SIZE-1);
    /* check if read() failed */
    if(br <= 0)
        goto close_clientfd;
    buffer[br] = '\0';
    if(strstr(buffer, "\r\n\r\n") == NULL)
        goto close_clientfd;

    char *filename = get_file_name(buffer);
    if(strcmp(filename, "/") == 0) {
        sprintf(filename, "index.html");
    }
    /* attempt to read html file */
    char *fullpath = calloc(1, strlen(static_dir) + strlen(filename) + 4);
    sprintf(fullpath, "./%s%s", static_dir, filename);
    printf("fullpath: %s\n", fullpath);
    int htmlfd = open(fullpath, O_RDONLY);
    free(fullpath);
    if(htmlfd < 0) {
        bw = write(clientfd, HTTP_404, sizeof(HTTP_404));
        goto close_clientfd;
    } else {
        char *filebuffer = calloc(1, FILE_BUFFER_SIZE);
        bw = write(clientfd, HTTP_OK, sizeof(HTTP_OK));
        if(bw <= 0)
            goto close_clientfd;
        while((br = read(htmlfd, filebuffer, FILE_BUFFER_SIZE-1))) {
            bw = write(clientfd, filebuffer, br);
            if(bw <= 0)
                goto close_clientfd;
            memset(filebuffer, 0, bw);
        }
        close(htmlfd);
        free(filebuffer);
        goto close_clientfd;
    }

close_clientfd:
    close(clientfd);
    return ;
}

int main(int argc, char *argv[])
{
    int port = default_port;
    int threads = default_poolsize;
    char *static_dir = default_static;

    /* Parse arguments */
    if(!parse_args(argc, argv, &port, &threads, &static_dir))
        exit(EXIT_FAILURE);

    /* check static directory */
    if(!check_static_dir(static_dir))
        exit(EXIT_FAILURE);

    /* initiate thread pool */

    /* open listen socket */
    int listenfd;
    if((listenfd = create_listen_socket(port)) < 0)
            exit(EXIT_FAILURE);

    /* Print server configuration */
    printf("Server running on port: %d\nthreads: %d\nstatic directory: %s\nType exit or quit to exit\n", port, threads, static_dir);

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
            if(handle_stdin())
                break;
        }

        /* handle new connection */
        if(FD_ISSET(listenfd, &readset)) {
            memset(&client_addr, 0, sizeof(client_addr));
            clientfd = accept(listenfd, (struct sockaddr*)&client_addr, &len);
            handle_connection(clientfd, client_addr, static_dir);
        }
    }

    /* Clean up */
    printf("stopping server\n");
    return EXIT_SUCCESS;
}
