#include <stdio.h>      /* printf(), perror(), fopen(), fread(), fclose() */
#include <stdlib.h>     /* EXIT_SUCCESS, EXIT_FAILURE, malloc(), free() */
#include <string.h>     /* strlen(), strcmp(), strstr(), strrchr(), memset() */
#include <unistd.h>     /* close() */
#include <arpa/inet.h>  /* sockaddr_in, htons(), htonl() */
#include <sys/types.h>  /* ssize_t */
#include <sys/socket.h> /* socket(), bind(), listen(), accept(), recv(), send() */

#define PORT 8080
#define BACKLOG 10
#define REQUEST_BUFFER_SIZE 4096
#define METHOD_SIZE 16
#define PATH_SIZE 256
#define FILE_PATH_SIZE 512

int create_server_socket(void);
void handle_client(int client_fd);
void send_response(int client_fd, const char *status_line, const char *content_type,
                   long content_length, const unsigned char *body);
void send_404(int client_fd);
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

        /* accept() blocks here until a browser or curl opens a connection. */
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            perror("accept failed");
            continue;
        }

        printf("Client connected.\n");

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
    char method[METHOD_SIZE];
    char path[PATH_SIZE];
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

    /* Pull the first two words from the request line.
       Example request line: GET /index.html HTTP/1.1
       After this, method is "GET" and path is "/index.html". */
    if (sscanf(request_buffer, "%15s %255s", method, path) != 2) {
        printf("Could not parse request line. Sending 404.\n");
        send_404(client_fd);
        return;
    }

    printf("Method: %s\n", method);
    printf("Requested path: %s\n", path);

    /* This tiny server only serves files for GET requests. */
    if (strcmp(method, "GET") != 0) {
        printf("Unsupported method: %s. Sending 404.\n\n", method);
        send_404(client_fd);
        return;
    }

    /* Ignore a query string so /style.css?v=1 still maps to /style.css. */
    query_string = strchr(path, '?');
    if (query_string != NULL) {
        *query_string = '\0';
    }

    /* Keep the file lookup inside ./public.
       This is still an educational server, so the rule is intentionally simple. */
    if (strstr(path, "..") != NULL) {
        printf("Blocked suspicious path: %s. Sending 404.\n\n", path);
        send_404(client_fd);
        return;
    }

    if (strcmp(path, "/") == 0) {
        snprintf(file_path, sizeof(file_path), "./public/index.html");
    } else {
        snprintf(file_path, sizeof(file_path), "./public%s", path);
    }

    serve_file(client_fd, file_path);
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

    printf("404 Not Found\n\n");

    send_response(client_fd, "HTTP/1.1 404 Not Found", "text/html",
                  (long)strlen(body), (const unsigned char *)body);
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
        printf("File not found: %s\n", file_path);
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

    printf("Serving file: %s (%ld bytes, %s)\n\n", file_path, file_size, mime_type);

    send_response(client_fd, "HTTP/1.1 200 OK", mime_type, file_size, file_contents);

    free(file_contents);
}
