#include <stdio.h>      /* printf(), perror(), fopen(), fread(), fclose() */
#include <stdlib.h>     /* EXIT_SUCCESS, EXIT_FAILURE, malloc(), free() */
#include <string.h>     /* strlen(), strcmp(), strchr(), strrchr(), strstr(), memset(), memcpy() */
#include <unistd.h>     /* close() */
#include <arpa/inet.h>  /* sockaddr_in, htons(), htonl() */
#include <sys/types.h>  /* ssize_t */
#include <sys/socket.h> /* socket(), bind(), listen(), accept(), recv(), send() */

#define PORT 8080
#define BACKLOG 10
#define REQUEST_BUFFER_SIZE 4096
#define FILE_PATH_SIZE 1024

typedef struct {
    char method[8];
    char path[512];
    char version[16];
} HttpRequest;

int create_server_socket(void);
void handle_client(int client_fd);
int parse_http_request(const char *raw_request, HttpRequest *request);
int validate_path(const char *path);
void log_request(const HttpRequest *request);
void log_response(const char *status, const char *file_path);
void send_response(int client_fd, const char *status_line, const char *content_type,
                   long content_length, const unsigned char *body);
void send_400(int client_fd);
void send_403(int client_fd);
void send_404(int client_fd);
void send_405(int client_fd);
const char *get_mime_type(const char *file_path);
void serve_file(int client_fd, const char *file_path);

int main(void)
{
    int server_fd;

    server_fd = create_server_socket();
    if (server_fd == -1) {
        return EXIT_FAILURE;
    }

    printf("Server listening on http://localhost:%d\n", PORT);
    printf("Serving files from the ./public directory.\n");
    printf("Press Ctrl+C to stop the server.\n\n");

    /* Keep accepting clients forever, one client at a time. */
    while (1) {
        int client_fd;
        struct sockaddr_in client_addr;
        socklen_t client_addr_length;
        char client_ip[INET_ADDRSTRLEN];

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

        handle_client(client_fd);

        /* This server closes each connection after one request and response. */
        close(client_fd);
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
    serve_file(client_fd, file_path);
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

void log_request(const HttpRequest *request)
{
    /* Structured logs make it easier to follow one request through the server. */
    printf("[REQUEST] %s %s %s\n", request->method, request->path, request->version);
}

void log_response(const char *status, const char *file_path)
{
    if (file_path != NULL) {
        printf("[RESPONSE] %s %s\n\n", status, file_path);
    } else {
        printf("[RESPONSE] %s\n\n", status);
    }
}

void send_response(int client_fd, const char *status_line, const char *content_type,
                   long content_length, const unsigned char *body)
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
    log_response("405 Method Not Allowed", NULL);
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

void serve_file(int client_fd, const char *file_path)
{
    FILE *file;
    long file_size;
    unsigned char *file_contents;
    size_t bytes_read;
    const char *mime_type;

    file = fopen(file_path, "rb");
    if (file == NULL) {
        printf("[INFO] File not found: %s\n", file_path);
        send_404(client_fd);
        return;
    }

    /* Move to the end to learn the file size, then move back to the start. */
    if (fseek(file, 0, SEEK_END) != 0) {
        perror("fseek failed");
        fclose(file);
        send_404(client_fd);
        return;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        perror("ftell failed");
        fclose(file);
        send_404(client_fd);
        return;
    }

    rewind(file);

    file_contents = malloc((size_t)file_size);
    if (file_size > 0 && file_contents == NULL) {
        perror("malloc failed");
        fclose(file);
        send_404(client_fd);
        return;
    }

    bytes_read = fread(file_contents, 1, (size_t)file_size, file);
    fclose(file);

    if (bytes_read != (size_t)file_size) {
        printf("Could not read entire file: %s\n", file_path);
        free(file_contents);
        send_404(client_fd);
        return;
    }

    mime_type = get_mime_type(file_path);

    printf("[INFO] Serving file: %s (%ld bytes, %s)\n", file_path, file_size, mime_type);

    send_response(client_fd, "HTTP/1.1 200 OK", mime_type, file_size, file_contents);
    log_response("200 OK", file_path);

    free(file_contents);
}
