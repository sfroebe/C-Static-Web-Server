#include <stdio.h>      /* printf(), perror() */
#include <stdlib.h>     /* EXIT_SUCCESS, EXIT_FAILURE */
#include <string.h>     /* strlen(), memset() */
#include <unistd.h>     /* close() */
#include <arpa/inet.h>  /* sockaddr_in, htons(), htonl() */
#include <sys/types.h>  /* ssize_t */
#include <sys/socket.h> /* socket(), bind(), listen(), accept(), recv(), send() */

int main(void)
{
    /* A socket file descriptor is an integer handle for a network socket. */
    int server_fd;

    /* This will hold the file descriptor for each connected browser/client. */
    int client_fd;

    /* This structure stores the address and port the server will listen on. */
    struct sockaddr_in server_addr;

    /* This buffer stores the raw HTTP request sent by the browser. */
    char request_buffer[4096];

    /* Create a TCP socket using IPv4.
       AF_INET means IPv4, SOCK_STREAM means TCP, and 0 chooses the default TCP protocol. */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket failed");
        return EXIT_FAILURE;
    }

    /* Clear the address structure so every byte starts as zero. */
    memset(&server_addr, 0, sizeof(server_addr));

    /* Use IPv4 addresses. */
    server_addr.sin_family = AF_INET;

    /* Listen on all network interfaces on this machine, such as 127.0.0.1. */
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* Listen on port 8080.
       htons() converts the port number into network byte order. */
    server_addr.sin_port = htons(8080);

    /* Attach the socket to the chosen IP address and port. */
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* Mark the socket as a listening socket.
       The number 10 is the backlog: how many connections may wait in line. */
    if (listen(server_fd, 10) == -1) {
        perror("listen failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on http://localhost:8080\n");
    printf("Press Ctrl+C to stop the server.\n\n");

    /* Keep accepting clients forever, one client at a time. */
    while (1) {
        ssize_t bytes_received;

        /* Wait for a client to connect.
           accept() blocks here until a browser or curl opens a connection. */
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            perror("accept failed");
            continue;
        }

        /* Read the client's HTTP request into our buffer.
           Leave one byte free so we can add a '\0' string terminator. */
        bytes_received = recv(client_fd, request_buffer, sizeof(request_buffer) - 1, 0);
        if (bytes_received == -1) {
            perror("recv failed");
            close(client_fd);
            continue;
        }

        /* Add a null terminator so printf can safely print the request as text. */
        request_buffer[bytes_received] = '\0';

        /* Print exactly what the browser sent, which is the raw HTTP request. */
        printf("----- Raw HTTP request -----\n");
        printf("%s\n", request_buffer);
        printf("----------------------------\n\n");

        /* This is the small HTML page the server sends back to the browser. */
        const char *body = "<!doctype html>\n"
                           "<html>\n"
                           "<head><title>C Server</title></head>\n"
                           "<body><h1>Hello from C server</h1></body>\n"
                           "</html>\n";

        /* This is a valid HTTP response: status line, headers, blank line, then HTML body. */
        char response[1024];

        /* Build the HTTP response and include the body length in bytes. */
        int response_length = snprintf(response, sizeof(response),
                                       "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/html\r\n"
                                       "Content-Length: %zu\r\n"
                                       "Connection: close\r\n"
                                       "\r\n"
                                       "%s",
                                       strlen(body), body);

        /* Send the response back to the connected client. */
        if (send(client_fd, response, response_length, 0) == -1) {
            perror("send failed");
        }

        /* Close this client connection before accepting the next one. */
        close(client_fd);
    }

    /* This line is never reached in this simple server, but it shows proper cleanup. */
    close(server_fd);

    return EXIT_SUCCESS;
}
