#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/un.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/stat.h>

int run_diagnostic(const char* self);
int run_proxy(int argc, const char* argv[]);
int run_update(int argc, const char* argv[]);
enum Transfer transfer_data(int from, int to);
int unpack_socket(struct msghdr* msg);
struct Message* unpack_message(struct msghdr* msg);
int send_message(int channel, struct Message* message, struct sockaddr_un* address);
int receive_message(int channel, struct Message* message, struct sockaddr_un* sender);

bool
is_fd_transferred(const struct Message* message);
struct Proxy {
    int proxy_listener;
    int input;
    int output;
    struct sockaddr_in input_peer;
    int api;
    struct sockaddr_un api_address;
};

enum Transfer {
    TRANSFER_OK,
    TRANSFER_CLOSED,
    TRANSFER_FAILED
};

enum Type {
    MESSAGE_REQUEST,
    MESSAGE_RESPONSE
};

enum Command {
    COMMAND_GET_PID,
    COMMAND_GET_API,
    COMMAND_GET_LISTENER,
    COMMAND_GET_INPUT,
    COMMAND_GET_OUTPUT,
    COMMAND_SHUDOWN
};

struct Message {
    enum Type type;
    enum Command command;
    union {
        pid_t pid;
        union { ;
            int listener;
            int input;
            int output;
            int fd;
        };
    };
};

int
main(int argc, const char* argv[]) {
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
                    "\t%s proxy FROM-PORT TO-PORT API-SOCKET\n"
                    "\t%s update API-SOCKET\n";
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

int
setup_api(const char* path, struct Proxy* proxy) {
    proxy->api = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (proxy->api == -1) {
        perror("error: unable: to allocate API socket");
        return EXIT_FAILURE;
    }

    memset(&proxy->api_address, 0, sizeof(proxy->api_address));
    proxy->api_address.sun_family = AF_UNIX;
    strcpy(proxy->api_address.sun_path, path);

    if (bind(proxy->api, (const struct sockaddr*)&proxy->api_address,
            sizeof(proxy->api_address)) < 0) {
        perror("error: api: bind");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int
setup_proxy(uint16_t from_port, uint16_t to_port, const char* api, struct Proxy* proxy) {
    if (setup_api(api, proxy) != EXIT_SUCCESS) {
        fprintf(stderr, "fatal: proxy: API setup failed\n");
        return EXIT_FAILURE;
    }

    proxy->proxy_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (proxy->proxy_listener == -1) {
        perror("error: unable to allocate socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(from_port);

    if (bind(proxy->proxy_listener, (const struct sockaddr*)&address,
            sizeof(address)) < 0) {
        perror("error: unable to bind to address");
        return EXIT_FAILURE;
    }

    if (listen(proxy->proxy_listener, 1) < 0) {
        perror("error: unable to listen for connections");
        return EXIT_FAILURE;
    }

    socklen_t peer_length = sizeof(proxy->input_peer);
    memset(&proxy->input_peer, 0, peer_length);
    proxy->input = accept(proxy->proxy_listener, (struct sockaddr*)&proxy->input_peer,
            &peer_length);
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

void
pack_socket(struct msghdr* msg, int fd) {
    struct cmsghdr* control = msg->msg_control;
    control->cmsg_level = SOL_SOCKET;
    control->cmsg_type = SCM_RIGHTS;
    control->cmsg_len = CMSG_LEN(sizeof(int));
    *((int*)CMSG_DATA(control)) = fd;
}

void
teardown_proxy(struct Proxy* proxy) {
    close(proxy->proxy_listener);
    close(proxy->input);
    close(proxy->output);
    close(proxy->api);
    unlink(proxy->api_address.sun_path);
}

int
handle_request(struct Proxy* proxy, struct Message* message) {
    switch (message->command) {
    case COMMAND_GET_PID:
        message->pid = getpid();
        break;
    case COMMAND_GET_LISTENER:
        message->listener = proxy->proxy_listener;
        break;
    case COMMAND_GET_INPUT:
        message->input = proxy->input;
        break;
    case COMMAND_GET_OUTPUT:
        message->output = proxy->output;
        break;
    case COMMAND_SHUDOWN:
        teardown_proxy(proxy);
        exit(EXIT_SUCCESS);
    default:
        fprintf(stderr, "error: api: method %d not implemented\n", message->command);
    }
    return EXIT_SUCCESS;
}

int
serve_api(struct Proxy* proxy) {
    struct Message message;
    struct sockaddr_un peer;

    receive_message(proxy->api, &message, &peer);

    if (handle_request(proxy, &message) != EXIT_SUCCESS) {
        fprintf(stderr, "error: api: unable to handle request\n");
        return EXIT_FAILURE;
    }

    message.type = MESSAGE_RESPONSE;
    if (send_message(proxy->api, &message, &peer)) {
        perror("api: sendmsg");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int
proxy_data(struct Proxy* proxy) {
    const int COUNT = 3;
    struct pollfd polled[COUNT];
    polled[0].fd = proxy->input;
    polled[0].events = POLLIN | POLLERR;
    polled[1].fd = proxy->output;
    polled[1].events = POLLIN | POLLERR;
    polled[2].fd = proxy->api;
    polled[2].events = POLLIN | POLLERR;

    int result = 0;
    while ((result = poll(polled, COUNT, -1)) > 0) {
        for (int i = 0; i < COUNT; i++) {
            if (polled[i].revents & POLLERR) {
                int error = EXIT_SUCCESS;
                socklen_t error_size = sizeof(error);
                getsockopt(polled[i].fd, SOL_SOCKET, SO_ERROR, &error, &error_size);
                fprintf(stderr, "error: %s (%d)", strerror(error), error);
                return EXIT_FAILURE;
            }
            if (polled[i].revents & POLLIN) {
                switch (i) {
                case 0:
                case 1:
                    result = transfer_data(polled[i].fd, polled[1 - i].fd);
                    if (result != 0) {
                        return result;
                    }
                    break;
                case 2:
                    result = serve_api(proxy);
                    break;
                }
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
    if (argc != 5) {
        return run_diagnostic(argv[0]);
    }

    const uint16_t from_port = parse_port(argv[2]);
    const uint16_t to_port = parse_port(argv[3]);
    if (!from_port || !to_port || (from_port == to_port)) {
        fprintf(stderr, "fatal: ports expected to be integers from 1 to 65535\n");
        return run_diagnostic(argv[0]);
    }

    const char* api = argv[4];

    struct Proxy proxy;
    if (setup_proxy(from_port, to_port, api, &proxy) != 0) {
        fprintf(stderr, "fatal: failed to setup proxy\n");
        return EXIT_FAILURE;
    }
    proxy_data(&proxy);
    teardown_proxy(&proxy);
}

int
unpack_socket(struct msghdr* msg) {
    struct cmsghdr* control = msg->msg_control;
    return *(int*)CMSG_DATA(control);
}

struct Message*
unpack_message(struct msghdr* msg) {
    struct Message* message = (struct Message*)msg->msg_iov[0].iov_base;
    if (message->type != MESSAGE_RESPONSE) {
        return message;
    }
    switch (message->command) {
    case COMMAND_GET_INPUT:
    case COMMAND_GET_OUTPUT:
    case COMMAND_GET_LISTENER:
        message->fd = unpack_socket(msg);
        break;
    }
    return message;
}

void
pack_message(struct Message* message, struct msghdr* msg) {
    if (message->type != MESSAGE_RESPONSE) {
        return;
    }
    switch (message->command) {
    case COMMAND_GET_INPUT:
    case COMMAND_GET_OUTPUT:
    case COMMAND_GET_LISTENER:
        pack_socket(msg, message->fd);
        break;
    }
}

int
send_message(int channel, struct Message* message, struct sockaddr_un* address) {
    struct msghdr msg;
    char control[CMSG_LEN(sizeof(int))];

    memset(&msg, 0, sizeof(msg));

    struct iovec iov;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_iov[0].iov_base = message;
    msg.msg_iov[0].iov_len = sizeof(*message);

    msg.msg_name = address;
    msg.msg_namelen = sizeof(*address);

    if (message->type == MESSAGE_RESPONSE) {
        if (is_fd_transferred(message)) {
            memset(&control, 0, sizeof(control));
            msg.msg_control = control;
            msg.msg_controllen = CMSG_LEN(sizeof(int));
        }
    }

    pack_message(message, &msg);
    if (sendmsg(channel, &msg, 0) < 0) {
        perror("update: error: sendmsg");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int
receive_message(int channel, struct Message* message, struct sockaddr_un* sender) {
    struct msghdr msg;
    char control[CMSG_LEN(sizeof(int))];

    memset(&msg, 0, sizeof(msg));

    struct iovec iov;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_iov[0].iov_base = message;
    msg.msg_iov[0].iov_len = sizeof(struct Message);

    if (sender != NULL) {
        msg.msg_name = sender;
        msg.msg_namelen = sizeof(*sender);
    }

    const bool has_attachment = is_fd_transferred(message);
    if (has_attachment) {
        memset(&control, 0, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = CMSG_LEN(sizeof(int));
    }

    if (recvmsg(channel, &msg, 0) < 0) {
        perror("update: error: recvmsg");
        return EXIT_FAILURE;
    }
    unpack_message(&msg);
    return EXIT_SUCCESS;
}

bool
is_fd_transferred(const struct Message* message) {
    const bool has_attachment = (message->command == COMMAND_GET_INPUT) ||
            (message->command == COMMAND_GET_OUTPUT) ||
            (message->command == COMMAND_GET_LISTENER);
    return has_attachment;
}

int
request(int channel, struct sockaddr_un* address,
        enum Command command, struct Message* message) {
    message->type = MESSAGE_REQUEST;
    message->command = command;
    if (send_message(channel, message, address) != EXIT_SUCCESS) {
        return EXIT_FAILURE;
    }
    return receive_message(channel, message, NULL);
}

int
request_fd(int channel, struct sockaddr_un* address,
        enum Command command, struct Message* message) {
    if (request(channel, address, command, message) != EXIT_SUCCESS) {
        return -1;
    }
    return message->fd;
}

int
run_update(int argc, const char** argv) {
    if (argc != 3) {
        return run_diagnostic(argv[0]);
    }
    const char* api_path = argv[2];

    int api = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (api == -1) {
        perror("update: socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;

    if (bind(api, (const struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("update: bind");
        return EXIT_FAILURE;
    }

    struct Message message;

    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, api_path);

    struct Proxy proxy;

    proxy.proxy_listener = request_fd(api, &address, COMMAND_GET_LISTENER, &message);
    proxy.input = request_fd(api, &address, COMMAND_GET_INPUT, &message);
    proxy.output = request_fd(api, &address, COMMAND_GET_OUTPUT, &message);

    fprintf(stderr, "debug: update: proxy_listener=%d\n", proxy.proxy_listener);
    fprintf(stderr, "debug: update: input=%d\n", proxy.input);
    fprintf(stderr, "debug: update: output=%d\n", proxy.output);

    struct sockaddr_in proxy_address;
    socklen_t proxy_address_length = sizeof(proxy_address);
    if (getsockname(proxy.proxy_listener, (struct sockaddr*)&proxy_address,
            &proxy_address_length) < 0) {
        perror("getsockname");
    } else {
        fprintf(stderr, "proxy address: %s:%d\n", inet_ntoa(proxy_address.sin_addr),
                ntohs(proxy_address.sin_port));
    }

    message.type = MESSAGE_REQUEST;
    message.command = COMMAND_SHUDOWN;
    send_message(api, &message, &address);

    close(api);

    fprintf(stderr, "info: waiting for API socket to be freed...\n");
    do {
        struct stat info;
        if (stat(api_path, &info) < 0) {
            if (errno == ENOENT) {
                break;
            }
            perror("stat");
            teardown_proxy(&proxy);
            return EXIT_FAILURE;
        }
        sleep(1);
    } while (true);

    fprintf(stderr, "info: restoring operations\n");
    setup_api(api_path, &proxy);
    proxy_data(&proxy);
    teardown_proxy(&proxy);
    return EXIT_SUCCESS;
}
