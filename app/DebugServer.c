#include "DebugServer.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static pthread_t debug_server_thread;
static bool debug_server_started;
static int debug_server_port;

static void write_response(int client, const char *body) {
    char header[256];
    int body_len = (int) strlen(body);
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              body_len);
    if (header_len > 0)
        (void) write(client, header, (size_t) header_len);
    (void) write(client, body, (size_t) body_len);
}

static void *debug_server_main(void *arg) {
    (void) arg;

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
        return NULL;

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t) debug_server_port);

    if (bind(server, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(server);
        return NULL;
    }
    if (listen(server, 4) < 0) {
        close(server);
        return NULL;
    }

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        char request[1024];
        (void) read(client, request, sizeof(request) - 1);
        write_response(client,
                       "{\"jsonrpc\":\"2.0\",\"result\":{\"status\":\"ok\"},\"id\":null}\n");
        close(client);
    }

    close(server);
    return NULL;
}

void debug_server_start(int port) {
    if (debug_server_started)
        return;

    debug_server_port = port;
    debug_server_started = true;
    if (pthread_create(&debug_server_thread, NULL, debug_server_main, NULL) == 0)
        pthread_detach(debug_server_thread);
}
