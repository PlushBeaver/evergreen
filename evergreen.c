#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>

int run_diagnostic(const char* self);
int run_proxy(int argc, const char* argv[]);
int run_update(int argc, const char* argv[]);

enum Transfer {
    TRANSFER_OK,
    TRANSFER_CLOSED,
    TRANSFER_FAILED
};


enum Transfer transfer_data(int from, int to);

int
main(int argc, char* argv[]) {
    if ((argc < 2) || (strcmp(argv[1], "-h") == 0)) {
        return run_diagnostic(argv[0]);
    }
    else if (strcmp(argv[1], "proxy") == 0) {
        return run_proxy(argc, argv);
    }
    else if (strcmp(argv[1], "update") == 0) {
        return run_update(argc, argv);
    }
    return run_diagnostic(argv[0]);
}

int
run_diagnostic(const char* self) {
    const char* message =
            "Usage:\n"
                    "\t%s proxy FROM-PORT TO-PORT\n"
                    "\t%s update PID\n";
    fprintf(stderr, message, self, self);
    return EXIT_FAILURE;
}

uint16_t
parse_port(const char* text) {
    int port = atoi(text);
    if ((port <= 0) || (port > 65535)) {
        return 0;
    }
    return (uint16_t)port;
}

struct Proxy {
    int listener;
    int input;
    int output;
    struct sockaddr_in input_peer;
};

int
setup_proxy(uint16_t from_port, uint16_t to_port, struct Proxy* proxy) {
    proxy->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (proxy->listener == -1) {
        perror("error: unable to allocate socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(from_port);

    if (bind(proxy->listener, (const struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("error: unable to bind to address");
        return EXIT_FAILURE;
    }

    if (listen(proxy->listener, 1) < 0) {
        perror("error: unable to listen for connections");
        return EXIT_FAILURE;
    }

    socklen_t peer_length = sizeof(proxy->input_peer);
    memset(&proxy->input_peer, 0, peer_length);
    proxy->input = accept(proxy->listener, (struct sockaddr*)&proxy->input_peer, &peer_length);
    if (proxy->input == -1) {
        perror("error: unable to accept connection");
        return EXIT_FAILURE;
    }

    proxy->output = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (proxy->output == -1) {
        perror("error: unable to allocate socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(to_port);

    puts("connecting");

    if (connect(proxy->output, (struct sockaddr*)&target, sizeof(target)) < 0) {
        perror("error: connect");
        return EXIT_FAILURE;
    }

    puts("connected");

    return EXIT_SUCCESS;
}

int
proxy_data(struct Proxy* proxy) {
    const int COUNT = 2;
    struct pollfd polled[COUNT];
    polled[0].fd = proxy->input;
    polled[0].events = POLLIN | POLLERR;
    polled[1].fd = proxy->output;
    polled[1].events = POLLIN | POLLERR;

    int result = 0;
    while ((result = poll(polled, COUNT, -1)) > 0) {
        for (int i = 0; i < COUNT; i++) {
            if (polled[i].revents & POLLIN) {
                result = transfer_data(polled[i].fd, polled[1 - i].fd);
                if (result != 0) {
                    return result;
                }
            }
            if (polled[i].revents & POLLERR) {
                int error = EXIT_SUCCESS;
                socklen_t error_size = sizeof(error);
                getsockopt(polled[i].fd, SOL_SOCKET, SO_ERROR, &error, &error_size);
                fprintf(stderr, "error: %s (%d)", strerror(error), error);
                return EXIT_FAILURE;
            }
        }
    }

    if (result < 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

enum Transfer
transfer_data(int from, int to) {
    char buffer[4096];
    const ssize_t received = recv(from, buffer, sizeof(buffer), 0);
    if (received == 0) {
        fprintf(stderr, "warning: connection closed\n");
        return TRANSFER_CLOSED;
    }
    else if (received < 0) {
        perror("error: unable to receive");
        return TRANSFER_FAILED;
    }

    int sent_total = 0;
    while (sent_total < received) {
        const ssize_t sent = send(to, buffer, received, 0);
        if (sent == 0) {
            fprintf(stderr, "warning: connection closed\n");
            return TRANSFER_CLOSED;
        }
        else if (sent < 0) {
            perror("error: unable to send");
            return TRANSFER_FAILED;
        }
        sent_total += sent;
    }
    return TRANSFER_OK;
}

int
run_proxy(int argc, const char** argv) {
    if (argc != 4) {
        return run_diagnostic(argv[0]);
    }

    const uint16_t from_port = parse_port(argv[2]);
    const uint16_t to_port = parse_port(argv[3]);
    if (!from_port || !to_port || (from_port == to_port)) {
        fprintf(stderr, "fatal: ports expected to be integers from 1 to 65535\n");
        return run_diagnostic(argv[0]);
    }

    struct Proxy proxy;
    if (setup_proxy(from_port, to_port, &proxy) != 0) {
        fprintf(stderr, "fatal: failed to setup proxy\n");
        return EXIT_FAILURE;
    }
    proxy_data(&proxy);
    close(proxy.listener);
    close(proxy.input);
    close(proxy.output);
}

int
run_update(int argc, const char** argv) {
    fprintf(stderr, "%s: fatal: not implemented\n", argv[0]);
    return EXIT_FAILURE;
}
