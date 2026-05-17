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

#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar) \
    for ((var) = SLIST_FIRST((head));              \
             (var) && ((tvar) = SLIST_NEXT((var), field), 1); \
                      (var) = (tvar))
#endif

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

// Global volatile flag for signal handler safety
volatile sig_atomic_t exit_requested = 0;

// Mutex protecting file writes and thread list modifications
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Structure for tracking thread metadata
typedef struct thread_data {
    pthread_t thread_id;
    int client_fd;
    char client_ip[INET6_ADDRSTRLEN];
    volatile int is_complete;
    SLIST_ENTRY(thread_data) entries;
} thread_data_t;

// Initialize the singly linked list head
SLIST_HEAD(slisthead, thread_data) head = SLIST_HEAD_INITIALIZER(head);

// Signal handler for graceful termination
static void signal_handler(int signal_number) {
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        exit_requested = 1;
    }
}

// Function to safely extract IP string from sockaddr structures
void get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen) {
    if (sa->sa_family == AF_INET) {
        inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr), s, maxlen);
    } else {
        inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr), s, maxlen);
    }
}

// Connection handler executed by each worker thread
void* connection_thread(void* thread_param) {
    thread_data_t *data = (thread_data_t *)thread_param;
    char *recv_buf = NULL;
    size_t total_bytes_received = 0;
    char chunk[BUFFER_SIZE];
    ssize_t bytes_read = 0;
    int newline_found = 0;

    // Stream communication loop until full packet (ending in \n) is read
    while (!newline_found && !exit_requested) {
        bytes_read = recv(data->client_fd, chunk, sizeof(chunk), 0);
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "Recv error occurred on socket");
            break;
        } else if (bytes_read == 0) {
            break; // Remote side closed the connection
        }

        // Dynamically resize internal buffer to match incoming stream
        char *new_buf = realloc(recv_buf, total_bytes_received + bytes_read);
        if (new_buf == NULL) {
            syslog(LOG_ERR, "Failed to allocate memory for incoming packet");
            free(recv_buf);
            close(data->client_fd);
            data->is_complete = 1;
            return NULL;
        }
        recv_buf = new_buf;
        memcpy(recv_buf + total_bytes_received, chunk, bytes_read);
        total_bytes_received += bytes_read;

        // Verify if full packet criteria has been reached
        if (recv_buf[total_bytes_received - 1] == '\n') {
            newline_found = 1;
        }
    }

    if (newline_found) {
        // Critical Section: Protect file manipulation across concurrent threads
        pthread_mutex_lock(&file_mutex);

        FILE *f = fopen(DATA_FILE, "a+");
        if (f != NULL) {
            // Append incoming packet atomically
            fwrite(recv_buf, 1, total_bytes_received, f);
            fflush(f);

            // Read the full historic contents back and send to user
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

    // Clean up connections and set completion marker
    free(recv_buf);
    close(data->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", data->client_ip);
    
    data->is_complete = 1;
    return NULL;
}

// Separate timestamp daemon thread initialized in the parent process
void* timestamp_thread(void* arg) {
    (void)arg;
    struct timespec ts;
    
    while (!exit_requested) {
        // Sleep using clock_nanosleep to prevent signal interruption errors
        ts.tv_sec = 10;
        ts.tv_nsec = 0;
        while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) && errno == EINTR) {
            if (exit_requested) return NULL;
        }

        time_t rawtime;
        struct tm *timeinfo;
        char time_str[100];
        char formatted_output[150];

        time(&rawtime);
        timeinfo = localtime(&rawtime);

        // Generate RFC 2822 compliant timestamp format
        strftime(time_str, sizeof(time_str), "%a, %d %b %Y %H:%M:%S %z", timeinfo);
        int len = snprintf(formatted_output, sizeof(formatted_output), "timestamp:%s\n", time_str);

        // Lock file access to ensure timestamps don't split packet streams
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

    // Setup signal actions
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Socket configuration steps
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
        syslog(LOG_ERR, "Failed to get address info");
        return -1;
    }

    int server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd < 0) {
        syslog(LOG_ERR, "Socket creation failed");
        freeaddrinfo(res);
        return -1;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) < 0) {
        syslog(LOG_ERR, "Binding failed");
        close(server_fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    if (listen(server_fd, 10) < 0) {
        syslog(LOG_ERR, "Listening failed");
        close(server_fd);
        return -1;
    }

    // Server forks into background if daemon parameter is applied
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid > 0) exit(0); // Parent process exits gracefully

        setsid();
        chdir("/");
        int dev_null = open("/dev/null", O_RDWR);
        dup2(dev_null, STDIN_FILENO);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        close(dev_null);
    }

    // Launch the timestamp generator right before accepting connections
    pthread_t time_tid;
    pthread_create(&time_tid, NULL, timestamp_thread, NULL);

    struct sockaddr_storage client_addr;
    socklen_t addr_size = sizeof(client_addr);

    // Main multi-threaded connection worker loop
    while (!exit_requested) {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_size);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, "Accept error");
            break;
        }

        thread_data_t *node = malloc(sizeof(thread_data_t));
        if (node == NULL) {
            syslog(LOG_ERR, "Node allocation failed");
            close(client_fd);
            continue;
        }

        node->client_fd = client_fd;
        node->is_complete = 0;
        get_ip_str((struct sockaddr *)&client_addr, node->client_ip, sizeof(node->client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", node->client_ip);

        // Spin up individual worker thread per connection
        if (pthread_create(&node->thread_id, NULL, connection_thread, node) != 0) {
            syslog(LOG_ERR, "Thread generation failed");
            close(client_fd);
            free(node);
            continue;
        }

        // Add node inside global single-linked listing under mutex tracking
        pthread_mutex_lock(&file_mutex);
        SLIST_INSERT_HEAD(&head, node, entries);
        pthread_mutex_unlock(&file_mutex);

        // Periodic, non-blocking cleanup loop for terminated context structures
        thread_data_t *curr, *tmp;
        pthread_mutex_lock(&file_mutex);
        SLIST_FOREACH_SAFE(curr, &head, entries, tmp) {
            if (curr->is_complete) {
                pthread_join(curr->thread_id, NULL); // Wait for thread finish
                SLIST_REMOVE(&head, curr, thread_data, entries);
                free(curr);
            }
        }
        pthread_mutex_unlock(&file_mutex);
    }

    // Program teardown initialization sequence
    pthread_join(time_tid, NULL);

    // Complete all remaining threads explicitly before shutdown context releases
    while (!SLIST_EMPTY(&head)) {
        thread_data_t *node = SLIST_FIRST(&head);
        pthread_join(node->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, entries);
        free(node);
    }

    pthread_mutex_destroy(&file_mutex);
    close(server_fd);
    unlink(DATA_FILE);
    syslog(LOG_INFO, "Application closed cleanly");
    closelog();

    return 0;
}
