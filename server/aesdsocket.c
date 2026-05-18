#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/queue.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>

#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar)           \
    for ((var) = SLIST_FIRST((head));                        \
         (var) && ((tvar) = SLIST_NEXT((var), field), 1);    \
         (var) = (tvar))
#endif

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

volatile sig_atomic_t exit_requested = 0;
int server_fd = -1;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct thread_data {
    pthread_t thread_id;
    int client_fd;
    char client_ip[INET6_ADDRSTRLEN];
    volatile int is_complete;
    SLIST_ENTRY(thread_data) entries;
} thread_data_t;

SLIST_HEAD(slisthead, thread_data) head = SLIST_HEAD_INITIALIZER(head);

static void signal_handler(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        exit_requested = 1;
    }
}

void get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen) {
    if (sa->sa_family == AF_INET) {
        inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), s, maxlen);
    } else {
        inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), s, maxlen);
    }
}

void* connection_thread(void* thread_param) {
    thread_data_t *data = (thread_data_t *)thread_param;
    char *recv_buf = NULL;
    size_t total_bytes_received = 0;
    char chunk[BUFFER_SIZE];
    ssize_t bytes_read = 0;
    int newline_found = 0;

    while (!newline_found && !exit_requested) {
        bytes_read = recv(data->client_fd, chunk, sizeof(chunk), 0);
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            break;
        } else if (bytes_read == 0) {
            break;
        }

        char *new_buf = realloc(recv_buf, total_bytes_received + bytes_read);
        if (new_buf == NULL) {
            free(recv_buf);
            close(data->client_fd);
            data->is_complete = 1;
            return NULL;
        }
        recv_buf = new_buf;
        memcpy(recv_buf + total_bytes_received, chunk, bytes_read);
        total_bytes_received += bytes_read;

        if (total_bytes_received > 0 && recv_buf[total_bytes_received - 1] == '\n') {
            newline_found = 1;
        }
    }

    if (newline_found && !exit_requested) {
        pthread_mutex_lock(&file_mutex);
        FILE *f = fopen(DATA_FILE, "a+");
        if (f != NULL) {
            fwrite(recv_buf, 1, total_bytes_received, f);
            fflush(f);
            fseek(f, 0, SEEK_SET);
            char send_buf[BUFFER_SIZE];
            size_t bytes_to_send;
            while ((bytes_to_send = fread(send_buf, 1, sizeof(send_buf), f)) > 0) {
                send(data->client_fd, send_buf, bytes_to_send, 0);
            }
            fclose(f);
        }
        pthread_mutex_unlock(&file_mutex);
    }

    free(recv_buf);
    close(data->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", data->client_ip);
    data->is_complete = 1;
    return NULL;
}

void* timestamp_thread(void* arg) {
    (void)arg;
    struct timespec ts;
    
    while (!exit_requested) {
        // Sleep in small 100ms intervals so we notice exit_requested quickly
        ts.tv_sec = 0;
        ts.tv_nsec = 100000000; 
        for (int i = 0; i < 100 && !exit_requested; i++) {
            while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL) && errno == EINTR) {
                if (exit_requested) return NULL;
            }
        }

        if (exit_requested) break;

        time_t rawtime;
        struct tm *timeinfo;
        char time_str[100];
        char formatted_output[150];

        time(&rawtime);
        timeinfo = localtime(&rawtime);
        strftime(time_str, sizeof(time_str), "%a, %d %b %Y %H:%M:%S %z", timeinfo);
        int len = snprintf(formatted_output, sizeof(formatted_output), "timestamp:%s\n", time_str);

        pthread_mutex_lock(&file_mutex);
        FILE *f = fopen(DATA_FILE, "a");
        if (f != NULL) {
            fwrite(formatted_output, 1, len, f);
            fclose(f);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
        closelog();
        return -1;
    }

    server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd < 0) {
        freeaddrinfo(res);
        closelog();
        return -1;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(server_fd);
        freeaddrinfo(res);
        closelog();
        return -1;
    }
    freeaddrinfo(res);

    if (listen(server_fd, 10) < 0) {
        close(server_fd);
        closelog();
        return -1;
    }

    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            close(server_fd);
            closelog();
            return -1;
        }
        if (pid > 0) {
            closelog();
            exit(0);
        }
        setsid();
        if (chdir("/") < 0) {}
        int dev_null = open("/dev/null", O_RDWR);
        if (dev_null >= 0) {
            dup2(dev_null, STDIN_FILENO);
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
    }

    pthread_t time_tid;
    pthread_create(&time_tid, NULL, timestamp_thread, NULL);

    struct sockaddr_storage client_addr;
    socklen_t addr_size = sizeof(client_addr);

    // Use select to make accept non-blocking
    while (!exit_requested) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        
        tv.tv_sec = 1; // Check exit_requested every 1 second
        tv.tv_usec = 0;

        int retval = select(server_fd + 1, &rfds, NULL, NULL, &tv);
        if (retval == -1) {
            if (errno == EINTR) continue;
            break;
        } else if (retval == 0) {
            continue; // Timeout, loop back and check exit_requested
        }

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_size);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        thread_data_t *node = malloc(sizeof(thread_data_t));
        if (node == NULL) {
            close(client_fd);
            continue;
        }

        node->client_fd = client_fd;
        node->is_complete = 0;
        get_ip_str((struct sockaddr *)&client_addr, node->client_ip, sizeof(node->client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", node->client_ip);

        if (pthread_create(&(node->thread_id), NULL, connection_thread, node) != 0) {
            close(client_fd);
            free(node);
            continue;
        }

        pthread_mutex_lock(&file_mutex);
        SLIST_INSERT_HEAD(&head, node, entries);
        pthread_mutex_unlock(&file_mutex);

        // Periodic list cleanup
        thread_data_t *tmp_node;
        thread_data_t *nxt_node;
        pthread_mutex_lock(&file_mutex);
        SLIST_FOREACH_SAFE(tmp_node, &head, entries, nxt_node) {
            if (tmp_node->is_complete) {
                SLIST_REMOVE(&head, tmp_node, thread_data, entries);
                pthread_join(tmp_node->thread_id, NULL);
                free(tmp_node);
            }
        }
        pthread_mutex_unlock(&file_mutex);
    }

    syslog(LOG_INFO, "Caught signal, exiting");

    // Join timer thread cleanly
    pthread_join(time_tid, NULL);

    // Join remaining connection threads cleanly
    thread_data_t *curr;
    while (!SLIST_EMPTY(&head)) {
        curr = SLIST_FIRST(&head);
        SLIST_REMOVE_HEAD(&head, entries);
        pthread_join(curr->thread_id, NULL);
        free(curr);
    }

    if (access(DATA_FILE, F_OK) == 0) {
        unlink(DATA_FILE);
    }
    if (server_fd >= 0) {
        close(server_fd);
    }

    closelog();
    return 0;
}
