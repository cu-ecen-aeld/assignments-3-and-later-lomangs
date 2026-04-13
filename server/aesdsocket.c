#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

int server_fd = -1;

void signal_handler(int sig) {
    syslog(LOG_INFO, "Caught signal, exiting");
    if (server_fd != -1) close(server_fd);
    unlink(DATA_FILE);
    closelog();
    exit(0);
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Signal Handling
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // B. Socket setup
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) return -1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Bind before forking (Requirement)
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        close(server_fd);
        return -1;
    }

    // DAEMON LOGIC
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            close(server_fd);
            return -1;
        }
        if (pid > 0) {
            // Parent exits
            exit(0);
        }
        // Child process continues: create new session and change directory
        if (setsid() < 0) exit(-1);
        if (chdir("/") < 0) exit(-1);
        
        // Redirect standard files to /dev/null
        int dev_null = open("/dev/null", O_RDWR);
        dup2(dev_null, STDIN_FILENO);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        close(dev_null);
    }

    if (listen(server_fd, 10) != 0) {
        close(server_fd);
        return -1;
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_size = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_size);
        if (client_fd == -1) continue;

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        int fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0666);
        char buf[BUFFER_SIZE];
        ssize_t nr;

        // Receive until newline
        while ((nr = recv(client_fd, buf, BUFFER_SIZE, 0)) > 0) {
            write(fd, buf, nr);
            if (memchr(buf, '\n', nr)) break;
        }

        // Send full file back
        lseek(fd, 0, SEEK_SET);
        while ((nr = read(fd, buf, BUFFER_SIZE)) > 0) {
            send(client_fd, buf, nr, 0);
        }

        close(fd);
        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }
    return 0;
}
