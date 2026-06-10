#include <stdio.h>      /* printf(), perror(), snprintf() */
#include <stdlib.h>     /* EXIT_SUCCESS, EXIT_FAILURE, malloc(), free() */
#include <string.h>     /* strlen(), strcmp(), strchr(), strrchr(), strstr(), memset(), memcpy() */
#include <errno.h>      /* errno */
#include <fcntl.h>      /* open(), O_RDONLY */
#include <time.h>       /* time(), time_t */
#include <unistd.h>     /* close() */
#include <pthread.h>    /* pthread_create(), pthread_detach(), pthread_self() */
#include <arpa/inet.h>  /* sockaddr_in, htons(), htonl() */
#include <sys/types.h>  /* ssize_t */
#include <sys/socket.h> /* socket(), bind(), listen(), accept(), recv(), send() */
#include <sys/sendfile.h> /* sendfile() */
#include <sys/stat.h>   /* fstat(), struct stat, S_ISREG() */

#define PORT 8080
#define BACKLOG 10
#define REQUEST_BUFFER_SIZE 4096
#define FILE_PATH_SIZE 1024

typedef struct {
    char method[8];
    char path[512];
    char version[16];
} HttpRequest;

typedef struct {
    unsigned long total_requests;
    unsigned long responses_200;
    unsigned long responses_403;
    unsigned long responses_404;
    unsigned long responses_405;
    unsigned long active_connections;
    time_t start_time;
} ServerStats;

ServerStats server_stats;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

int create_server_socket(void);
void *client_thread(void *arg);
void handle_client(int client_fd);
int parse_http_request(const char *raw_request, HttpRequest *request);
int validate_path(const char *path);
void initialize_stats(void);
void increment_request_count(void);
void increment_response_count(int status_code);
void increment_active_connections(void);
void decrement_active_connections(void);
ServerStats get_stats_snapshot(void);
void log_request(const HttpRequest *request);
void log_response(const char *status, const char *file_path);
int send_header(int client_fd, const char *status_line, const char *content_type,
                long content_length);
void send_response(int client_fd, const char *status_line, const char *content_type,
                   long content_length, const unsigned char *body);
void send_400(int client_fd);
void send_500(int client_fd);
void send_403(int client_fd);
void send_404(int client_fd);
void send_405(int client_fd);
void serve_stats_page(int client_fd);
const char *get_mime_type(const char *file_path);
void serve_file_sendfile(int client_fd, const char *file_path);

int main(void)
{
    int server_fd;

    initialize_stats();

    server_fd = create_server_socket();
    if (server_fd == -1) {
        return EXIT_FAILURE;
    }

    printf("Server listening on http://localhost:%d\n", PORT);
    printf("Serving files from the ./public directory.\n");
    printf("Press Ctrl+C to stop the server.\n\n");

    /* Keep accepting clients forever.

       In the earlier single-threaded version, the server did this:

           accept one client -> handle that client -> accept the next client

       That is simple, but one slow client can make every other client wait.

       In this threaded version, the main thread only accepts new connections.
       Each client is handed to a worker thread, so several requests can be in
       progress at the same time. This is called thread-per-connection
       concurrency. It is easy to understand, but it is not infinitely scalable:
       thousands of clients would mean thousands of operating-system threads. */
    while (1) {
        int client_fd;
        int *client_fd_ptr;
        struct sockaddr_in client_addr;
        socklen_t client_addr_length;
        char client_ip[INET_ADDRSTRLEN];
        pthread_t thread_id;
        int pthread_result;

        /* accept() blocks here until a browser or curl opens a connection. */
        client_addr_length = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_length);
        if (client_fd == -1) {
            perror("accept failed");
            continue;
        }

        /* inet_ntoa() turns the client's IPv4 address into readable text. */
        snprintf(client_ip, sizeof(client_ip), "%s", inet_ntoa(client_addr.sin_addr));
        printf("[INFO] Client connected: %s\n", client_ip);

        /* Give the client socket to a worker thread.

           IMPORTANT: do not pass &client_fd directly to pthread_create().
           client_fd is a stack variable that gets reused on the next loop
           iteration. If the main thread accepts another client before the
           worker thread copies the value, both threads could look at the same
           changing variable. That race can make a worker handle the wrong
           socket, or a socket that has already been replaced.

           Allocating one int per connection gives each worker its own stable
           copy. The worker frees this memory after copying out the fd value. */
        client_fd_ptr = malloc(sizeof(*client_fd_ptr));
        if (client_fd_ptr == NULL) {
            perror("malloc failed");
            close(client_fd);
            continue;
        }

        *client_fd_ptr = client_fd;

        /* pthread_create() starts client_thread() in a new thread.
           The fourth argument is the one void* value passed to that function. */
        pthread_result = pthread_create(&thread_id, NULL, client_thread, client_fd_ptr);
        if (pthread_result != 0) {
            printf("[ERROR] pthread_create failed: %s\n", strerror(pthread_result));
            free(client_fd_ptr);
            close(client_fd);
            continue;
        }

        /* A detached thread cleans up its own thread resources when it exits.
           That keeps this small server from needing to remember every thread
           and call pthread_join() later. */
        pthread_result = pthread_detach(thread_id);
        if (pthread_result != 0) {
            printf("[ERROR] pthread_detach failed: %s\n", strerror(pthread_result));
        }

        printf("[THREAD] Spawned worker thread %lu for %s\n",
               (unsigned long)thread_id, client_ip);
    }

    /* This line is never reached in this simple server, but it shows proper cleanup. */
    close(server_fd);

    return EXIT_SUCCESS;
}

int create_server_socket(void)
{
    int server_fd;
    struct sockaddr_in server_addr;

    /* Create a TCP socket using IPv4.
       AF_INET means IPv4, SOCK_STREAM means TCP, and 0 chooses the default TCP protocol. */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket failed");
        return -1;
    }

    /* Clear the address structure so every byte starts as zero. */
    memset(&server_addr, 0, sizeof(server_addr));

    /* Use IPv4 addresses. */
    server_addr.sin_family = AF_INET;

    /* Listen on all network interfaces on this machine, such as 127.0.0.1. */
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* htons() converts the port number into network byte order. */
    server_addr.sin_port = htons(PORT);

    /* Attach the socket to the chosen IP address and port. */
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    /* Mark the socket as a listening socket.
       BACKLOG is how many connections may wait in line. */
    if (listen(server_fd, BACKLOG) == -1) {
        perror("listen failed");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

void *client_thread(void *arg)
{
    int client_fd;

    /* The main accept loop allocated this int so the worker gets a stable
       socket descriptor, not the address of a changing stack variable. */
    client_fd = *(int *)arg;
    free(arg);

    increment_active_connections();
    printf("[THREAD %lu] Handling client connection\n", (unsigned long)pthread_self());

    handle_client(client_fd);

    /* This server closes each connection after one request and response.
       Closing here matters because the worker thread owns the client socket
       once pthread_create() succeeds. */
    close(client_fd);

    decrement_active_connections();
    printf("[THREAD %lu] Client connection closed\n", (unsigned long)pthread_self());

    return NULL;
}

void handle_client(int client_fd)
{
    char request_buffer[REQUEST_BUFFER_SIZE];
    HttpRequest request;
    char file_path[FILE_PATH_SIZE];
    char *query_string;
    ssize_t bytes_received;

    /* Read the client's HTTP request into our buffer.
       Leave one byte free so we can add a '\0' string terminator. */
    bytes_received = recv(client_fd, request_buffer, sizeof(request_buffer) - 1, 0);
    if (bytes_received == -1) {
        perror("recv failed");
        return;
    }

    /* Add a null terminator so sscanf() and printf() can treat it as text. */
    request_buffer[bytes_received] = '\0';
    increment_request_count();

    /* A request struct keeps the parsed pieces together.
       That makes later code easier to read than passing method, path, and version
       around as separate local variables. */
    if (!parse_http_request(request_buffer, &request)) {
        printf("[ERROR] Failed to parse HTTP request\n");
        send_400(client_fd);
        return;
    }

    log_request(&request);

    /* This tiny server only serves files for GET requests.
       405 means "the path may exist, but this HTTP method is not allowed here." */
    if (strcmp(request.method, "GET") != 0) {
        printf("[ERROR] Unsupported method: %s\n", request.method);
        send_405(client_fd);
        return;
    }

    /* Ignore a query string so /style.css?v=1 still maps to /style.css. */
    query_string = strchr(request.path, '?');
    if (query_string != NULL) {
        *query_string = '\0';
    }

    /* /stats is a dynamic route, not a file in ./public.
       We handle it before building a filesystem path so a request for /stats
       becomes a live HTML report about this running process. */
    if (strcmp(request.path, "/stats") == 0) {
        printf("[STATS] Serving dynamic statistics page\n");
        serve_stats_page(client_fd);
        return;
    }

    /* Path traversal is when a request tries to escape the web directory.
       For example, /../../etc/passwd asks the server to walk upward in the
       filesystem. This simple validation keeps requests inside ./public. */
    if (!validate_path(request.path)) {
        printf("[SECURITY] Path validation failed: %s\n", request.path);
        send_403(client_fd);
        return;
    }

    if (strcmp(request.path, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "./public/index.html");
    } else {
        snprintf(file_path, sizeof(file_path), "./public%s", request.path);
    }

    printf("[INFO] Filesystem path: %s\n", file_path);
    serve_file_sendfile(client_fd, file_path);
}

int parse_http_request(const char *raw_request, HttpRequest *request)
{
    char request_line[REQUEST_BUFFER_SIZE];
    char extra[16];
    size_t line_length;

    /* The first line of an HTTP request is:
       METHOD path VERSION

       Example:
       GET /index.html HTTP/1.1

       We copy only the first line before calling sscanf(). That matters because
       a real HTTP request has more header lines after the request line.

       sscanf() stops at whitespace, so it naturally reads those three words.
       The width limits (%7s, %511s, %15s) protect our fixed-size arrays. */
    memset(request, 0, sizeof(*request));
    memset(request_line, 0, sizeof(request_line));

    line_length = strcspn(raw_request, "\r\n");
    if (line_length == 0 || line_length >= sizeof(request_line)) {
        return 0;
    }

    memcpy(request_line, raw_request, line_length);

    if (sscanf(request_line, "%7s %511s %15s %15s",
               request->method, request->path, request->version, extra) != 3) {
        return 0;
    }

    return 1;
}

int validate_path(const char *path)
{
    size_t i;
    size_t path_length;

    if (path == NULL) {
        return 0;
    }

    path_length = strlen(path);

    /* Empty, extremely long, or non-absolute paths are malformed for this server. */
    if (path_length == 0 || path_length >= 512 || path[0] != '/') {
        return 0;
    }

    /* ".." can mean "go up one directory", which can expose files outside ./public. */
    if (strstr(path, "..") != NULL) {
        return 0;
    }

    /* Some clients or attackers may encode dots as %2e or %2E.
       For example, /%2e%2e/server.c means the same thing as /../server.c
       after URL decoding. This beginner server does not implement full URL
       decoding yet, so we take the conservative educational approach and
       reject encoded dots before they can become path traversal later. */
    for (i = 0; i + 2 < path_length; i++) {
        if (path[i] == '%' && path[i + 1] == '2' &&
            (path[i + 2] == 'e' || path[i + 2] == 'E')) {
            return 0;
        }
    }

    /* Repeated slashes are not needed for this small server and can hide odd paths. */
    if (strstr(path, "//") != NULL) {
        return 0;
    }

    /* Reject a few characters that make paths harder to reason about in a lesson. */
    for (i = 0; i < path_length; i++) {
        if (path[i] == '\\' || path[i] == ' ' || path[i] == '\t' ||
            path[i] == '\r' || path[i] == '\n') {
            return 0;
        }
    }

    return 1;
}

void initialize_stats(void)
{
    /* Server statistics are shared state: every worker thread can read or
       update these counters at the same time.

       A race condition happens when two threads touch the same data at once
       and the final result depends on unlucky timing. For example, if two
       threads both read total_requests as 10, both add 1, and both store 11,
       the server lost one request count.

       A mutex is a small lock. Only one thread may hold it at a time, so the
       read-modify-write sequence for each counter stays correct and easy to
       reason about. */
    pthread_mutex_lock(&stats_mutex);
    memset(&server_stats, 0, sizeof(server_stats));
    server_stats.start_time = time(NULL);
    pthread_mutex_unlock(&stats_mutex);
}

void increment_request_count(void)
{
    pthread_mutex_lock(&stats_mutex);
    server_stats.total_requests++;
    pthread_mutex_unlock(&stats_mutex);
}

void increment_response_count(int status_code)
{
    pthread_mutex_lock(&stats_mutex);

    if (status_code == 200) {
        server_stats.responses_200++;
    } else if (status_code == 403) {
        server_stats.responses_403++;
    } else if (status_code == 404) {
        server_stats.responses_404++;
    } else if (status_code == 405) {
        server_stats.responses_405++;
    }

    pthread_mutex_unlock(&stats_mutex);
}

void increment_active_connections(void)
{
    pthread_mutex_lock(&stats_mutex);
    server_stats.active_connections++;
    pthread_mutex_unlock(&stats_mutex);
}

void decrement_active_connections(void)
{
    pthread_mutex_lock(&stats_mutex);
    if (server_stats.active_connections > 0) {
        server_stats.active_connections--;
    }
    pthread_mutex_unlock(&stats_mutex);
}

ServerStats get_stats_snapshot(void)
{
    ServerStats snapshot;

    /* Copy the whole struct while holding the mutex, then release the lock.
       The /stats page can format HTML from this local snapshot without making
       other request threads wait on slow string formatting or network I/O. */
    pthread_mutex_lock(&stats_mutex);
    snapshot = server_stats;
    pthread_mutex_unlock(&stats_mutex);

    return snapshot;
}

void log_request(const HttpRequest *request)
{
    /* Structured logs make it easier to follow one request through the server. */
    printf("[REQUEST][thread %lu] %s %s %s\n",
           (unsigned long)pthread_self(), request->method, request->path, request->version);
}

void log_response(const char *status, const char *file_path)
{
    if (file_path != NULL) {
        printf("[RESPONSE][thread %lu] %s %s\n\n",
               (unsigned long)pthread_self(), status, file_path);
    } else {
        printf("[RESPONSE][thread %lu] %s\n\n",
               (unsigned long)pthread_self(), status);
    }
}

int send_header(int client_fd, const char *status_line, const char *content_type,
                long content_length)
{
    char header[512];
    int header_length;

    /* HTTP responses start with headers, then a blank line, then the body bytes. */
    header_length = snprintf(header, sizeof(header),
                             "%s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %ld\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             status_line, content_type, content_length);

    if (send(client_fd, header, header_length, 0) == -1) {
        perror("send header failed");
        return 0;
    }

    return 1;
}

void send_response(int client_fd, const char *status_line, const char *content_type,
                   long content_length, const unsigned char *body)
{
    if (!send_header(client_fd, status_line, content_type, content_length)) {
        return;
    }

    if (content_length > 0 && body != NULL) {
        if (send(client_fd, body, (size_t)content_length, 0) == -1) {
            perror("send body failed");
        }
    }
}

void send_400(int client_fd)
{
    const char *body = "<!doctype html>\n"
                       "<html>\n"
                       "<head><title>400 Bad Request</title></head>\n"
                       "<body>\n"
                       "<h1>400 Bad Request</h1>\n"
                       "<p>The server could not understand the HTTP request line.</p>\n"
                       "</body>\n"
                       "</html>\n";

    /* 400 is for malformed requests, such as a missing method/path/version. */
    send_response(client_fd, "HTTP/1.1 400 Bad Request", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    log_response("400 Bad Request", NULL);
}

void send_500(int client_fd)
{
    const char *body = "<!doctype html>\n"
                       "<html>\n"
                       "<head><title>500 Internal Server Error</title></head>\n"
                       "<body>\n"
                       "<h1>500 Internal Server Error</h1>\n"
                       "<p>The server had trouble sending the requested file.</p>\n"
                       "</body>\n"
                       "</html>\n";

    send_response(client_fd, "HTTP/1.1 500 Internal Server Error", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    log_response("500 Internal Server Error", NULL);
}

void send_403(int client_fd)
{
    const char *body = "<!doctype html>\n"
                       "<html>\n"
                       "<head><title>403 Forbidden</title></head>\n"
                       "<body>\n"
                       "<h1>403 Forbidden</h1>\n"
                       "<p>The requested path is not allowed by this server.</p>\n"
                       "</body>\n"
                       "</html>\n";

    /* 403 is for requests we understood but chose not to allow. */
    send_response(client_fd, "HTTP/1.1 403 Forbidden", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    increment_response_count(403);
    log_response("403 Forbidden", NULL);
}

void send_404(int client_fd)
{
    const char *body = "<!doctype html>\n"
                       "<html>\n"
                       "<head><title>404 Not Found</title></head>\n"
                       "<body>\n"
                       "<h1>404 Not Found</h1>\n"
                       "<p>The requested file was not found.</p>\n"
                       "</body>\n"
                       "</html>\n";

    /* 404 is for valid-looking paths that do not map to an existing file. */
    send_response(client_fd, "HTTP/1.1 404 Not Found", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    increment_response_count(404);
    log_response("404 Not Found", NULL);
}

void send_405(int client_fd)
{
    const char *body = "<!doctype html>\n"
                       "<html>\n"
                       "<head><title>405 Method Not Allowed</title></head>\n"
                       "<body>\n"
                       "<h1>405 Method Not Allowed</h1>\n"
                       "<p>This educational server only supports GET requests.</p>\n"
                       "</body>\n"
                       "</html>\n";

    send_response(client_fd, "HTTP/1.1 405 Method Not Allowed", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    increment_response_count(405);
    log_response("405 Method Not Allowed", NULL);
}

void serve_stats_page(int client_fd)
{
    ServerStats snapshot;
    char body[2048];
    int body_length;
    time_t now;
    long uptime_seconds;

    snapshot = get_stats_snapshot();
    now = time(NULL);

    /* Uptime is the current time minus the time recorded at server startup.
       time_t stores calendar time in seconds on POSIX systems, which is good
       enough for a simple educational status page. */
    uptime_seconds = (long)(now - snapshot.start_time);

    body_length = snprintf(body, sizeof(body),
                           "<!doctype html>\n"
                           "<html>\n"
                           "<head><title>Server Stats</title></head>\n"
                           "<body>\n"
                           "<h1>Server Statistics</h1>\n"
                           "<p>Total Requests: %lu</p>\n"
                           "<p>Active Connections: %lu</p>\n"
                           "<p>200 Responses: %lu</p>\n"
                           "<p>404 Responses: %lu</p>\n"
                           "<p>403 Responses: %lu</p>\n"
                           "<p>405 Responses: %lu</p>\n"
                           "<p>Uptime: %ld seconds</p>\n"
                           "</body>\n"
                           "</html>\n",
                           snapshot.total_requests,
                           snapshot.active_connections,
                           snapshot.responses_200,
                           snapshot.responses_404,
                           snapshot.responses_403,
                           snapshot.responses_405,
                           uptime_seconds);

    if (body_length < 0 || body_length >= (int)sizeof(body)) {
        send_500(client_fd);
        return;
    }

    send_response(client_fd, "HTTP/1.1 200 OK", "text/html",
                  body_length, (const unsigned char *)body);
    increment_response_count(200);
    log_response("200 OK", "/stats");
}

const char *get_mime_type(const char *file_path)
{
    const char *extension;

    /* strrchr() finds the last dot, which usually starts the file extension. */
    extension = strrchr(file_path, '.');

    if (extension == NULL) {
        return "application/octet-stream";
    }

    if (strcmp(extension, ".html") == 0) {
        return "text/html";
    }
    if (strcmp(extension, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(extension, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(extension, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(extension, ".jpg") == 0 || strcmp(extension, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(extension, ".txt") == 0) {
        return "text/plain";
    }

    return "application/octet-stream";
}

void serve_file_sendfile(int client_fd, const char *file_path)
{
    int file_fd;
    struct stat file_info;
    off_t offset;
    off_t remaining_bytes;
    ssize_t bytes_sent;
    const char *mime_type;

    /* The older version of this server used:

           fopen() -> malloc() -> fread() -> send() -> free()

       That approach is easy to understand, but it copies the whole file from
       the kernel into this program's user-space memory, then send() copies
       those bytes back into the kernel so the network stack can transmit them.

       Linux sendfile() is a more direct path. It asks the kernel to move bytes
       from a file descriptor to a socket descriptor. At a high level, this is
       called "zero-copy" because the file contents do not need to be copied
       into our own malloc'd buffer first. That can reduce CPU work and memory
       use, especially for large files or many simultaneous clients.

       We still keep the code beginner-friendly: open the file, ask fstat()
       for its size, send normal HTTP headers, then stream the body with
       sendfile(). */
    file_fd = open(file_path, O_RDONLY);
    if (file_fd == -1) {
        printf("[INFO] File not found: %s\n", file_path);
        send_404(client_fd);
        return;
    }

    /* fstat() reads metadata for the already-open file descriptor.
       We need st_size for the HTTP Content-Length header. */
    if (fstat(file_fd, &file_info) == -1) {
        perror("fstat failed");
        close(file_fd);
        send_500(client_fd);
        return;
    }

    /* This static server is meant to serve regular files, not directories,
       devices, or other special filesystem objects. */
    if (!S_ISREG(file_info.st_mode)) {
        printf("[INFO] Not a regular file: %s\n", file_path);
        close(file_fd);
        send_404(client_fd);
        return;
    }

    mime_type = get_mime_type(file_path);

    printf("[FILE] %s\n", file_path);
    printf("[FILE] Size: %ld bytes\n", (long)file_info.st_size);
    printf("[FILE] MIME: %s\n", mime_type);

    if (!send_header(client_fd, "HTTP/1.1 200 OK", mime_type, (long)file_info.st_size)) {
        close(file_fd);
        return;
    }

    offset = 0;
    remaining_bytes = file_info.st_size;

    while (remaining_bytes > 0) {
        bytes_sent = sendfile(client_fd, file_fd, &offset, (size_t)remaining_bytes);

        if (bytes_sent == -1) {
            if (errno == EINTR) {
                continue;
            }

            /* If sendfile() fails after the 200 OK header has already been
               sent, HTTP does not let us take that header back and replace it
               with a clean error response. For this educational server we log
               the failure clearly and close the connection. */
            perror("sendfile failed");
            close(file_fd);
            return;
        }

        if (bytes_sent == 0) {
            break;
        }

        remaining_bytes -= bytes_sent;
    }

    close(file_fd);

    increment_response_count(200);
    log_response("200 OK", file_path);
}
