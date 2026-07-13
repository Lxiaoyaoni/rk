#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVICES 64
#define READ_BUFFER_SIZE 16384

struct device_state {
    int running;
    int ok;
    int returncode;
    char stage[64];
    char message[256];
    char updated_at[32];
};

struct recover_job {
    int device_id;
};

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct device_state states[MAX_DEVICES];
static char script_path[512] = "./restart_one_rk_bridge.sh";
static char config_path[512] = "./rk_bridge_config.sh";
static const char *listen_host = "0.0.0.0";
static int listen_port = 18110;
static volatile int running = 1;

static void now_text(char *buf, size_t size) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void set_state(int device_id, int is_running, int ok, int returncode, const char *stage, const char *message) {
    if (device_id < 0 || device_id >= MAX_DEVICES) return;
    pthread_mutex_lock(&state_mutex);
    states[device_id].running = is_running;
    states[device_id].ok = ok;
    states[device_id].returncode = returncode;
    snprintf(states[device_id].stage, sizeof(states[device_id].stage), "%s", stage ? stage : "");
    snprintf(states[device_id].message, sizeof(states[device_id].message), "%s", message ? message : "");
    now_text(states[device_id].updated_at, sizeof(states[device_id].updated_at));
    pthread_mutex_unlock(&state_mutex);
}

static int get_running(int device_id) {
    if (device_id < 0 || device_id >= MAX_DEVICES) return 0;
    pthread_mutex_lock(&state_mutex);
    int value = states[device_id].running;
    pthread_mutex_unlock(&state_mutex);
    return value;
}

static void send_response(int fd, int code, const char *status, const char *content_type, const char *body) {
    if (!body) body = "";
    char header[512];
    int body_len = (int)strlen(body);
    int n = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        code,
        status,
        content_type,
        body_len
    );
    if (n > 0) send(fd, header, (size_t)n, MSG_NOSIGNAL);
    if (body_len > 0) send(fd, body, (size_t)body_len, MSG_NOSIGNAL);
}

static void send_json(int fd, const char *body) {
    send_response(fd, 200, "OK", "application/json; charset=utf-8", body);
}

static void send_error_json(int fd, int code, const char *status, const char *message) {
    char body[512];
    snprintf(body, sizeof(body), "{\"status\":\"failed\",\"message\":\"%s\"}\n", message ? message : "error");
    send_response(fd, code, status, "application/json; charset=utf-8", body);
}

static int ascii_lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static const char *find_header_name(const char *text, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return text;
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && ascii_lower((unsigned char)p[i]) == ascii_lower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return p;
    }
    return NULL;
}

static int parse_content_length(const char *request) {
    const char *p = find_header_name(request, "Content-Length:");
    if (!p) return 0;
    p += strlen("Content-Length:");
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static int parse_device_id(const char *body) {
    const char *p = strstr(body, "\"device\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    int id = -1;
    if (sscanf(p, "%d", &id) != 1) return -1;
    return id;
}

static void *recover_thread(void *arg) {
    struct recover_job *job = (struct recover_job *)arg;
    int device_id = job->device_id;
    free(job);

    char device_text[32];
    snprintf(device_text, sizeof(device_text), "%d", device_id);
    set_state(device_id, 1, 0, -1, "running", "recover script started");

    pid_t pid = fork();
    if (pid < 0) {
        set_state(device_id, 0, 0, errno, "error", strerror(errno));
        return NULL;
    }

    if (pid == 0) {
        execl("/bin/bash", "bash", script_path, device_text, config_path, (char *)NULL);
        perror("exec recover script failed");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        set_state(device_id, 0, 0, errno, "error", strerror(errno));
        return NULL;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        set_state(device_id, 0, 1, 0, "waiting_user_start_stream", "bridge restarted; tap Start screen stream on phone if needed");
    } else {
        int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        set_state(device_id, 0, 0, rc, "failed", "recover script failed");
    }
    return NULL;
}

static void status_json(char *body, size_t size) {
    size_t used = 0;
    used += (size_t)snprintf(body + used, size - used, "{\"devices\":{");
    int first = 1;
    pthread_mutex_lock(&state_mutex);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (states[i].updated_at[0] == '\0') continue;
        used += (size_t)snprintf(
            body + used,
            size > used ? size - used : 0,
            "%s\"%d\":{\"running\":%s,\"ok\":%s,\"returncode\":%d,\"stage\":\"%s\",\"message\":\"%s\",\"updated_at\":\"%s\"}",
            first ? "" : ",",
            i,
            states[i].running ? "true" : "false",
            states[i].ok ? "true" : "false",
            states[i].returncode,
            states[i].stage,
            states[i].message,
            states[i].updated_at
        );
        first = 0;
        if (used + 256 >= size) break;
    }
    pthread_mutex_unlock(&state_mutex);
    snprintf(body + used, size > used ? size - used : 0, "}}\n");
}

static void handle_client(int fd) {
    char request[READ_BUFFER_SIZE];
    int total = 0;
    while (total + 1 < (int)sizeof(request)) {
        ssize_t n = recv(fd, request + total, sizeof(request) - 1 - (size_t)total, 0);
        if (n <= 0) break;
        total += (int)n;
        request[total] = '\0';
        char *body = strstr(request, "\r\n\r\n");
        if (body) {
            int header_len = (int)(body + 4 - request);
            int content_length = parse_content_length(request);
            if (total >= header_len + content_length) break;
        }
    }
    request[total] = '\0';

    char method[16] = {0};
    char path[256] = {0};
    sscanf(request, "%15s %255s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        send_json(fd, "{\"status\":\"ok\"}\n");
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/status") == 0) {
        char body[8192];
        status_json(body, sizeof(body));
        send_json(fd, body);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/recover") == 0) {
        char *body = strstr(request, "\r\n\r\n");
        if (!body) {
            send_error_json(fd, 400, "Bad Request", "missing body");
            return;
        }
        body += 4;
        int device_id = parse_device_id(body);
        if (device_id < 0 || device_id >= MAX_DEVICES) {
            send_error_json(fd, 400, "Bad Request", "invalid device");
            return;
        }
        if (get_running(device_id)) {
            char response[256];
            snprintf(response, sizeof(response), "{\"status\":\"already_running\",\"device\":%d}\n", device_id);
            send_json(fd, response);
            return;
        }

        set_state(device_id, 1, 0, -1, "queued", "recover queued");
        struct recover_job *job = (struct recover_job *)calloc(1, sizeof(*job));
        if (!job) {
            set_state(device_id, 0, 0, errno, "error", "out of memory");
            send_error_json(fd, 500, "Internal Server Error", "out of memory");
            return;
        }
        job->device_id = device_id;
        pthread_t tid;
        if (pthread_create(&tid, NULL, recover_thread, job) != 0) {
            free(job);
            set_state(device_id, 0, 0, errno, "error", "pthread_create failed");
            send_error_json(fd, 500, "Internal Server Error", "pthread_create failed");
            return;
        }
        pthread_detach(tid);

        char response[256];
        snprintf(response, sizeof(response), "{\"status\":\"queued\",\"device\":%d}\n", device_id);
        send_json(fd, response);
        return;
    }

    send_error_json(fd, 404, "Not Found", "unknown endpoint");
}

static void *client_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    handle_client(fd);
    close(fd);
    return NULL;
}

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static void usage(const char *argv0) {
    printf("Usage: %s [--host 0.0.0.0] [--port 18110] [--script ./restart_one_rk_bridge.sh] [--config ./rk_bridge_config.sh]\n", argv0);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            listen_host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            listen_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            snprintf(script_path, sizeof(script_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            snprintf(config_path, sizeof(config_path), "%s", argv[++i]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)listen_port);
    if (strcmp(listen_host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, listen_host, &addr.sin_addr) != 1) {
        printf("invalid host: %s\n", listen_host);
        close(server);
        return 1;
    }

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(server);
        return 1;
    }
    if (listen(server, 16) != 0) {
        perror("listen");
        close(server);
        return 1;
    }

    printf("RK recover service listening on %s:%d\n", listen_host, listen_port);
    printf("script: %s\n", script_path);
    printf("config: %s\n", config_path);
    fflush(stdout);

    while (running) {
        int *client = (int *)malloc(sizeof(int));
        if (!client) break;
        *client = accept(server, NULL, NULL);
        if (*client < 0) {
            free(client);
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, client) != 0) {
            close(*client);
            free(client);
            continue;
        }
        pthread_detach(tid);
    }

    close(server);
    return 0;
}
