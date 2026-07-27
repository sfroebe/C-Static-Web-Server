#include <stdio.h>      /* printf(), perror(), snprintf() */
#include <stdlib.h>     /* EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>     /* strlen(), strcmp(), strchr(), strrchr(), strstr(), memset(), memcpy() */
#include <errno.h>      /* errno */
#include <stdint.h>     /* uint32_t */
#include <fcntl.h>      /* open(), O_RDONLY, fcntl() */
#include <time.h>       /* time(), time_t */
#include <unistd.h>     /* close() */
#include <arpa/inet.h>  /* sockaddr_in, htons(), htonl() */
#include <sys/types.h>  /* ssize_t */
#include <sys/socket.h> /* socket(), bind(), listen(), accept(), recv(), send() */
#include <sys/sendfile.h> /* sendfile() */
#include <sys/stat.h>   /* fstat(), struct stat, S_ISREG() */
#include <sys/epoll.h>  /* epoll_create1(), epoll_ctl(), epoll_wait() */

#define PORT 8080
/* BACKLOG is the kernel queue for TCP handshakes waiting for accept(). It is
   not a limit on active clients. A value of 10 was smaller than the benchmark
   concurrency of 50, so connection bursts waited or retried before epoll could
   accept them. Keep this comfortably above expected concurrent connections. */
#define BACKLOG 128
#define REQUEST_BUFFER_SIZE 4096
#define FILE_PATH_SIZE 1024
#define MAX_EPOLL_EVENTS 256
#define RESPONSE_BUFFER_SIZE 4096

/* Set to 1 while learning or debugging. The benchmark script overrides this
   to 0 so terminal output does not become the server's bottleneck. Keeping
   the default at 1 preserves the educational trace during normal use. */
#ifndef ENABLE_LOGGING
#define ENABLE_LOGGING 1
#endif

#if ENABLE_LOGGING
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

typedef struct {
    char method[8];
    char path[512];
    char version[16];
} HttpRequest;

typedef struct {
    unsigned long total_requests;
    unsigned long responses_200;
    unsigned long responses_400;
    unsigned long responses_403;
    unsigned long responses_404;
    unsigned long responses_405;
    unsigned long active_connections;
    time_t start_time;
} ServerStats;

/* One small state object belongs to each connected client. epoll stores a
   pointer to this object in event.data.ptr, so the event loop can resume a
   partial read or write without blocking or searching for the socket. */
typedef struct {
    int fd;
    char request_buffer[REQUEST_BUFFER_SIZE];
    size_t request_length;
    unsigned char response_buffer[RESPONSE_BUFFER_SIZE];
    size_t response_length;
    size_t response_sent;
    int file_fd;
    off_t file_offset;
    off_t file_remaining;
} ClientConnection;

ServerStats server_stats;

int create_server_socket(void);
int set_nonblocking(int fd);
int create_epoll(void);
int add_epoll_listener(int epoll_fd, int server_fd);
int add_epoll_fd(int epoll_fd, ClientConnection *client);
int modify_epoll_fd(int epoll_fd, ClientConnection *client, uint32_t events);
int remove_epoll_fd(int epoll_fd, int fd);
void close_client_connection(int epoll_fd, ClientConnection *client);
void accept_new_connections(int epoll_fd, int server_fd);
void process_client_socket(int epoll_fd, ClientConnection *client);
void event_loop(int epoll_fd, int server_fd);
void handle_client(ClientConnection *client);
int flush_client_response(ClientConnection *client);
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
int send_header(ClientConnection *client, const char *status_line, const char *content_type,
                long content_length);
void send_response(ClientConnection *client, const char *status_line, const char *content_type,
                   long content_length, const unsigned char *body);
void send_400(ClientConnection *client);
void send_500(ClientConnection *client);
void send_403(ClientConnection *client);
void send_404(ClientConnection *client);
void send_405(ClientConnection *client);
void serve_stats_page(ClientConnection *client);
const char *get_mime_type(const char *file_path);
void serve_file_sendfile(ClientConnection *client, const char *file_path);

int main(void)
{
    int server_fd;
    int epoll_fd;

    initialize_stats();

    server_fd = create_server_socket();
    if (server_fd == -1) {
        return EXIT_FAILURE;
    }

    /* epoll is Linux's readiness-notification interface. Instead of creating
       one thread per client and letting each thread block in recv(), this one
       thread asks the kernel which descriptors are ready right now.

       epoll does not perform I/O for us. An event simply says that an operation
       such as accept() or recv() is unlikely to block at this moment. */
    epoll_fd = create_epoll();
    if (epoll_fd == -1) {
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (!set_nonblocking(server_fd)) {
        close(epoll_fd);
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (!add_epoll_listener(epoll_fd, server_fd)) {
        close(epoll_fd);
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on http://localhost:%d\n", PORT);
    printf("Serving files from the ./public directory.\n");
    printf("Press Ctrl+C to stop the server.\n\n");

    event_loop(epoll_fd, server_fd);

    close(epoll_fd);
    close(server_fd);

    return EXIT_SUCCESS;
}

int set_nonblocking(int fd)
{
    int flags;

    /* A blocking recv(), accept(), or sendfile() can pause the only event-loop
       thread. While it waits for one slow client, no other ready client is
       handled. O_NONBLOCK makes these calls return EAGAIN instead, so the loop
       stays in control and can return to epoll_wait(). */
    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl O_NONBLOCK failed");
        return 0;
    }

    return 1;
}

int create_epoll(void)
{
    int epoll_fd = epoll_create1(0);

    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
    }

    return epoll_fd;
}

int add_epoll_listener(int epoll_fd, int server_fd)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    /* NULL identifies the listening socket; client events hold a pointer. */
    event.data.ptr = NULL;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl ADD listener failed");
        return 0;
    }

    return 1;
}

int add_epoll_fd(int epoll_fd, ClientConnection *client)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = client;

    /* EPOLLIN asks for read readiness. We deliberately do not use EPOLLET:
       level-triggered epoll keeps reporting a descriptor while it remains
       readable, which is easier to reason about than edge-triggered mode. */
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client->fd, &event) == -1) {
        perror("epoll_ctl ADD failed");
        return 0;
    }

    return 1;
}

int modify_epoll_fd(int epoll_fd, ClientConnection *client, uint32_t events)
{
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.ptr = client;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &event) == -1) {
        perror("epoll_ctl MOD failed");
        return 0;
    }

    return 1;
}

int remove_epoll_fd(int epoll_fd, int fd)
{
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1 && errno != ENOENT) {
        perror("epoll_ctl DEL failed");
        return 0;
    }

    return 1;
}

void accept_new_connections(int epoll_fd, int server_fd)
{
    while (1) {
        int client_fd;
        ClientConnection *client;
        struct sockaddr_in client_addr;
        socklen_t client_addr_length = sizeof(client_addr);
        char client_ip[INET_ADDRSTRLEN];

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_length);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* All currently pending connections were accepted. */
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept failed");
            return;
        }

        snprintf(client_ip, sizeof(client_ip), "%s", inet_ntoa(client_addr.sin_addr));
        LOG("[EPOLL] Accepted client: %s\n", client_ip);

        /* malloc avoids clearing two fixed-size buffers that recv()/snprintf()
           immediately overwrite. Explicitly initialize only state fields. */
        client = malloc(sizeof(*client));
        if (client == NULL) {
            perror("malloc client failed");
            close(client_fd);
            continue;
        }

        client->fd = client_fd;
        client->request_length = 0;
        client->response_length = 0;
        client->response_sent = 0;
        client->file_fd = -1;
        client->file_offset = 0;
        client->file_remaining = 0;

        if (!set_nonblocking(client_fd) || !add_epoll_fd(epoll_fd, client)) {
            close(client_fd);
            free(client);
            continue;
        }

        increment_active_connections();
    }
}

void close_client_connection(int epoll_fd, ClientConnection *client)
{
    int client_fd = client->fd;

    remove_epoll_fd(epoll_fd, client_fd);
    close(client_fd);
    if (client->file_fd != -1) {
        close(client->file_fd);
    }
    free(client);
    decrement_active_connections();
    LOG("[EPOLL] Closing connection: fd=%d\n", client_fd);
}

void process_client_socket(int epoll_fd, ClientConnection *client)
{
    ssize_t bytes_received;

    /* Read until the request headers arrive or the non-blocking socket says
       EAGAIN. The buffer is retained in ClientConnection if TCP split the
       request across packets. */
    while (client->request_length < sizeof(client->request_buffer) - 1) {
        bytes_received = recv(client->fd,
                              client->request_buffer + client->request_length,
                              sizeof(client->request_buffer) - 1 - client->request_length,
                              0);
        if (bytes_received > 0) {
            client->request_length += (size_t)bytes_received;
            client->request_buffer[client->request_length] = '\0';

            if (strstr(client->request_buffer, "\r\n\r\n") != NULL ||
                strstr(client->request_buffer, "\n\n") != NULL) {
                LOG("[EPOLL] Client socket ready: fd=%d\n", client->fd);
                handle_client(client);

                if (flush_client_response(client) == 1) {
                    close_client_connection(epoll_fd, client);
                } else if (!modify_epoll_fd(epoll_fd, client, EPOLLOUT)) {
                    close_client_connection(epoll_fd, client);
                }
                return;
            }
            continue;
        }

        if (bytes_received == 0) {
            close_client_connection(epoll_fd, client);
            return;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        perror("recv failed");
        close_client_connection(epoll_fd, client);
        return;
    }

    /* A full buffer without an end-of-headers marker is malformed. */
    increment_request_count();
    send_400(client);
    if (flush_client_response(client) == 1) {
        close_client_connection(epoll_fd, client);
    } else if (!modify_epoll_fd(epoll_fd, client, EPOLLOUT)) {
        close_client_connection(epoll_fd, client);
    }
}

void event_loop(int epoll_fd, int server_fd)
{
    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (1) {
        int ready_count;
        int i;

        /* epoll_wait() sleeps without consuming CPU until the kernel has one
           or more events. It returns only ready descriptors, rather than
           making us scan every connected socket ourselves. */
        ready_count = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        if (ready_count == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait failed");
            return;
        }

        for (i = 0; i < ready_count; i++) {
            ClientConnection *client = events[i].data.ptr;

            if (client == NULL) {
                LOG("[EPOLL] Listening socket ready\n");
                accept_new_connections(epoll_fd, server_fd);
            } else if (events[i].events & EPOLLIN) {
                /* Check readable data first. A client can send a request and
                   close its write side in the same event, so EPOLLIN may be
                   present together with a hangup flag. */
                process_client_socket(epoll_fd, client);
            } else if (events[i].events & EPOLLOUT) {
                if (flush_client_response(client) == 1) {
                    close_client_connection(epoll_fd, client);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    close_client_connection(epoll_fd, client);
                }
            } else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                /* A peer that disconnected has no request left for us to read. */
                LOG("[EPOLL] Client error or hangup: fd=%d\n", client->fd);
                close_client_connection(epoll_fd, client);
            }
        }
    }
}

int create_server_socket(void)
{
    int server_fd;
    int reuse_address;
    struct sockaddr_in server_addr;

    /* Create a TCP socket using IPv4.
       AF_INET means IPv4, SOCK_STREAM means TCP, and 0 chooses the default TCP protocol. */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket failed");
        return -1;
    }

    /* Allow the server to restart right away after a crash or Ctrl+C.
       Without this, the operating system may keep port 8080 reserved for a
       short TIME_WAIT period, which can make bind() fail with:

           Address already in use

       This does not steal the port from another still-running server; it only
       lets this process reuse the port once the previous socket is gone. */
    reuse_address = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        perror("setsockopt SO_REUSEADDR failed");
        close(server_fd);
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

void handle_client(ClientConnection *client)
{
    HttpRequest request;
    char file_path[FILE_PATH_SIZE];
    char *query_string;

    /* process_client_socket() already collected a complete request header.
       The buffer lives with the connection so a partial TCP read is preserved. */
    increment_request_count();

    /* A request struct keeps the parsed pieces together.
       That makes later code easier to read than passing method, path, and version
       around as separate local variables. */
    if (!parse_http_request(client->request_buffer, &request)) {
        printf("[ERROR] Failed to parse HTTP request\n");
        send_400(client);
        return;
    }

    log_request(&request);

    /* This tiny server only serves files for GET requests.
       405 means "the path may exist, but this HTTP method is not allowed here." */
    if (strcmp(request.method, "GET") != 0) {
        printf("[ERROR] Unsupported method: %s\n", request.method);
        send_405(client);
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
        LOG("[STATS] Serving dynamic statistics page\n");
        serve_stats_page(client);
        return;
    }

    /* Path traversal is when a request tries to escape the web directory.
       For example, /../../etc/passwd asks the server to walk upward in the
       filesystem. This simple validation keeps requests inside ./public. */
    if (!validate_path(request.path)) {
        LOG("[SECURITY] Path validation failed: %s\n", request.path);
        send_403(client);
        return;
    }

    if (strcmp(request.path, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "./public/index.html");
    } else {
        snprintf(file_path, sizeof(file_path), "./public%s", request.path);
    }

    LOG("[INFO] Filesystem path: %s\n", file_path);
    serve_file_sendfile(client, file_path);
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
    /* The old pthread server needed a mutex because many worker threads could
       update these values simultaneously. The epoll event loop is one thread,
       so each update finishes before the next event is processed. That removes
       the race condition and makes mutexes unnecessary for this architecture. */
    memset(&server_stats, 0, sizeof(server_stats));
    server_stats.start_time = time(NULL);
}

void increment_request_count(void)
{
    server_stats.total_requests++;
}

void increment_response_count(int status_code)
{
    if (status_code == 200) {
        server_stats.responses_200++;
    } else if (status_code == 400) {
        server_stats.responses_400++;
    } else if (status_code == 403) {
        server_stats.responses_403++;
    } else if (status_code == 404) {
        server_stats.responses_404++;
    } else if (status_code == 405) {
        server_stats.responses_405++;
    }

}

void increment_active_connections(void)
{
    server_stats.active_connections++;
}

void decrement_active_connections(void)
{
    if (server_stats.active_connections > 0) {
        server_stats.active_connections--;
    }
}

ServerStats get_stats_snapshot(void)
{
    ServerStats snapshot;

    /* A local snapshot keeps the formatting code separate from the counters.
       No lock is needed: the one event-loop thread cannot concurrently change
       server_stats while it is already executing this function. */
    snapshot = server_stats;

    return snapshot;
}

void log_request(const HttpRequest *request)
{
    /* Structured logs make it easier to follow one request through the server. */
    LOG("[REQUEST] %s %s %s\n", request->method, request->path, request->version);
}

void log_response(const char *status, const char *file_path)
{
    if (file_path != NULL) {
        LOG("[RESPONSE] %s %s\n\n", status, file_path);
    } else {
        LOG("[RESPONSE] %s\n\n", status);
    }
}

int send_header(ClientConnection *client, const char *status_line, const char *content_type,
                long content_length)
{
    int header_length;

    /* Queue headers rather than sending them immediately. If the socket's
       output buffer fills, EPOLLOUT will resume this exact byte position. */
    client->response_sent = 0;
    client->response_length = 0;
    header_length = snprintf((char *)client->response_buffer,
                             sizeof(client->response_buffer),
                             "%s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %ld\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             status_line, content_type, content_length);

    if (header_length < 0 || header_length >= (int)sizeof(client->response_buffer)) {
        perror("response header too large");
        return 0;
    }

    client->response_length = (size_t)header_length;
    return 1;
}

void send_response(ClientConnection *client, const char *status_line, const char *content_type,
                   long content_length, const unsigned char *body)
{
    if (!send_header(client, status_line, content_type, content_length)) {
        return;
    }

    if (content_length > 0 && body != NULL &&
        client->response_length + (size_t)content_length <= sizeof(client->response_buffer)) {
        memcpy(client->response_buffer + client->response_length, body, (size_t)content_length);
        client->response_length += (size_t)content_length;
    } else if (content_length > 0) {
        /* All built-in error and stats pages fit in RESPONSE_BUFFER_SIZE. */
        perror("response body too large");
    }
}

/* Return 1 when every queued byte has been sent, 0 when epoll must wait for
   EPOLLOUT, or -1 on a real socket/file error. This is the key difference from
   the earlier synchronous response path: a slow receiver no longer blocks the
   whole server and no response bytes are discarded on EAGAIN. */
int flush_client_response(ClientConnection *client)
{
    ssize_t bytes_sent;

    while (client->response_sent < client->response_length) {
        bytes_sent = send(client->fd,
                          client->response_buffer + client->response_sent,
                          client->response_length - client->response_sent,
                          MSG_NOSIGNAL);
        if (bytes_sent > 0) {
            client->response_sent += (size_t)bytes_sent;
            continue;
        }
        if (bytes_sent == -1 && errno == EINTR) {
            continue;
        }
        if (bytes_sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        perror("send response failed");
        return -1;
    }

    while (client->file_remaining > 0) {
        bytes_sent = sendfile(client->fd, client->file_fd, &client->file_offset,
                              (size_t)client->file_remaining);
        if (bytes_sent > 0) {
            client->file_remaining -= bytes_sent;
            continue;
        }
        if (bytes_sent == -1 && errno == EINTR) {
            continue;
        }
        if (bytes_sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        perror("sendfile failed");
        return -1;
    }

    if (client->file_fd != -1) {
        close(client->file_fd);
        client->file_fd = -1;
    }

    return 1;
}

void send_400(ClientConnection *client)
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
    send_response(client, "HTTP/1.1 400 Bad Request", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    increment_response_count(400);
    log_response("400 Bad Request", NULL);
}

void send_500(ClientConnection *client)
{
    const char *body = "<!doctype html>\n"
                       "<html>\n"
                       "<head><title>500 Internal Server Error</title></head>\n"
                       "<body>\n"
                       "<h1>500 Internal Server Error</h1>\n"
                       "<p>The server had trouble sending the requested file.</p>\n"
                       "</body>\n"
                       "</html>\n";

    send_response(client, "HTTP/1.1 500 Internal Server Error", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    log_response("500 Internal Server Error", NULL);
}

void send_403(ClientConnection *client)
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
    send_response(client, "HTTP/1.1 403 Forbidden", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    increment_response_count(403);
    log_response("403 Forbidden", NULL);
}

void send_404(ClientConnection *client)
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
    send_response(client, "HTTP/1.1 404 Not Found", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    increment_response_count(404);
    log_response("404 Not Found", NULL);
}

void send_405(ClientConnection *client)
{
    const char *body = "<!doctype html>\n"
                       "<html>\n"
                       "<head><title>405 Method Not Allowed</title></head>\n"
                       "<body>\n"
                       "<h1>405 Method Not Allowed</h1>\n"
                       "<p>This educational server only supports GET requests.</p>\n"
                       "</body>\n"
                       "</html>\n";

    send_response(client, "HTTP/1.1 405 Method Not Allowed", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
    increment_response_count(405);
    log_response("405 Method Not Allowed", NULL);
}

void serve_stats_page(ClientConnection *client)
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
                           "<p>400 Responses: %lu</p>\n"
                           "<p>404 Responses: %lu</p>\n"
                           "<p>403 Responses: %lu</p>\n"
                           "<p>405 Responses: %lu</p>\n"
                           "<p>Uptime: %ld seconds</p>\n"
                           "</body>\n"
                           "</html>\n",
                           snapshot.total_requests,
                           snapshot.active_connections,
                           snapshot.responses_200,
                           snapshot.responses_400,
                           snapshot.responses_404,
                           snapshot.responses_403,
                           snapshot.responses_405,
                           uptime_seconds);

    if (body_length < 0 || body_length >= (int)sizeof(body)) {
        send_500(client);
        return;
    }

    send_response(client, "HTTP/1.1 200 OK", "text/html",
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

void serve_file_sendfile(ClientConnection *client, const char *file_path)
{
    int file_fd;
    struct stat file_info;
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
        LOG("[INFO] File not found: %s\n", file_path);
        send_404(client);
        return;
    }

    /* fstat() reads metadata for the already-open file descriptor.
       We need st_size for the HTTP Content-Length header. */
    if (fstat(file_fd, &file_info) == -1) {
        perror("fstat failed");
        close(file_fd);
        send_500(client);
        return;
    }

    /* This static server is meant to serve regular files, not directories,
       devices, or other special filesystem objects. */
    if (!S_ISREG(file_info.st_mode)) {
        LOG("[INFO] Not a regular file: %s\n", file_path);
        close(file_fd);
        send_404(client);
        return;
    }

    mime_type = get_mime_type(file_path);

    LOG("[FILE] %s\n", file_path);
    LOG("[FILE] Size: %ld bytes\n", (long)file_info.st_size);
    LOG("[FILE] MIME: %s\n", mime_type);

    if (!send_header(client, "HTTP/1.1 200 OK", mime_type, (long)file_info.st_size)) {
        close(file_fd);
        return;
    }

    /* Do not loop on sendfile() here. On a non-blocking socket it may return
       EAGAIN, and spinning would stall every other client. flush_client_response()
       keeps this state and resumes after epoll reports EPOLLOUT. */
    client->file_fd = file_fd;
    client->file_offset = 0;
    client->file_remaining = file_info.st_size;

    increment_response_count(200);
    log_response("200 OK", file_path);
}
