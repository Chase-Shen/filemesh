#ifndef SERVER_UTILS_H
#define SERVER_UTILS_H

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUFFER_SIZE 65536
#define FILEMESH_PATH_SIZE 1024
#define FILEMESH_MAX_BATCH_FILES 32
#define FILEMESH_GATEWAY_PORT 11111
#define FILEMESH_PDF_PORT 22222
#define FILEMESH_TEXT_PORT 33333
#define FILEMESH_ZIP_PORT 44444
#define FILEMESH_HOST "127.0.0.1"

enum Operation {
    OP_UPLOAD = 1,
    OP_DOWNLOAD = 2,
    OP_REMOVE = 3,
    OP_TAR = 4,
    OP_DISPLAY = 5
};

static inline ssize_t send_bytes(int sockfd, const char *buffer, size_t length) {
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t bytes_sent = send(sockfd, buffer + total_sent, length - total_sent, 0);
        if (bytes_sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return -1;
        }
        if (bytes_sent == 0) {
            errno = EPIPE;
            return -1;
        }
        total_sent += (size_t)bytes_sent;
    }
    return (ssize_t)total_sent;
}

static inline ssize_t recv_bytes(int sockfd, char *buffer, size_t length) {
    size_t total_received = 0;

    while (total_received < length) {
        ssize_t bytes_received = recv(sockfd, buffer + total_received,
                                      length - total_received, 0);
        if (bytes_received < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            return -1;
        }
        if (bytes_received == 0) {
            break;
        }
        total_received += (size_t)bytes_received;
    }
    return (ssize_t)total_received;
}

static inline int send_int(int sockfd, int value) {
    uint32_t network_value = htonl((uint32_t)(int32_t)value);
    return send_bytes(sockfd, (const char *)&network_value, sizeof(network_value)) ==
                   (ssize_t)sizeof(network_value)
               ? 0
               : -1;
}

static inline int recv_int(int sockfd, int *value) {
    uint32_t network_value;
    if (recv_bytes(sockfd, (char *)&network_value, sizeof(network_value)) !=
        (ssize_t)sizeof(network_value)) {
        return -1;
    }
    *value = (int32_t)ntohl(network_value);
    return 0;
}

static inline int send_string(int sockfd, const char *value) {
    size_t length = strlen(value);
    if (length > INT_MAX || send_int(sockfd, (int)length) < 0) {
        return -1;
    }
    return send_bytes(sockfd, value, length) == (ssize_t)length ? 0 : -1;
}

static inline int recv_string(int sockfd, char *buffer, size_t buffer_size) {
    int length;
    if (recv_int(sockfd, &length) < 0 || length < 0 ||
        (size_t)length >= buffer_size) {
        return -1;
    }
    if (recv_bytes(sockfd, buffer, (size_t)length) != length) {
        return -1;
    }
    buffer[length] = '\0';
    return 0;
}

static inline int recv_file_size(int sockfd, const char *filename) {
    int file_size;
    if (recv_int(sockfd, &file_size) < 0) {
        printf("Error: Failed to receive file size for '%s'\n", filename);
        return -1;
    }
    if (file_size < 0) {
        printf("File '%s' not found on server\n", filename);
        return -1;
    }
    return file_size;
}

static inline int send_file_data(int sockfd, const char *filepath) {
    FILE *file = fopen(filepath, "rb");
    if (file == NULL) {
        send_int(sockfd, -1);
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        send_int(sockfd, -1);
        return -1;
    }
    long file_size = ftell(file);
    if (file_size < 0 || file_size > INT_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        send_int(sockfd, -1);
        return -1;
    }
    if (send_int(sockfd, (int)file_size) < 0) {
        fclose(file);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (send_bytes(sockfd, buffer, bytes_read) < 0) {
            fclose(file);
            return -1;
        }
    }

    int status = ferror(file) ? -1 : 0;
    fclose(file);
    return status;
}

static inline int recv_file_data(int sockfd, const char *filepath, int file_size) {
    if (file_size < 0) {
        return -1;
    }

    FILE *file = fopen(filepath, "wb");
    if (file == NULL) {
        return -1;
    }

    char buffer[BUFFER_SIZE];
    int bytes_remaining = file_size;
    int total_received = 0;

    while (bytes_remaining > 0) {
        size_t chunk_size = bytes_remaining < BUFFER_SIZE
                                ? (size_t)bytes_remaining
                                : (size_t)BUFFER_SIZE;
        ssize_t bytes_received = recv_bytes(sockfd, buffer, chunk_size);
        if (bytes_received <= 0 ||
            fwrite(buffer, 1, (size_t)bytes_received, file) !=
                (size_t)bytes_received) {
            fclose(file);
            unlink(filepath);
            return -1;
        }
        bytes_remaining -= (int)bytes_received;
        total_received += (int)bytes_received;
    }

    fclose(file);
    return total_received;
}

static inline int connect_to_server(const char *server_ip, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_address.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }
    if (connect(sockfd, (struct sockaddr *)&server_address,
                sizeof(server_address)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }
    return sockfd;
}

static inline int initialize_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    int option = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) < 0) {
        perror("setsockopt");
        close(sockfd);
        return -1;
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_address.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    return sockfd;
}

static inline int create_directory(const char *path) {
    size_t length = strlen(path);
    if (length == 0 || length >= FILEMESH_PATH_SIZE) {
        return -1;
    }

    char temporary[FILEMESH_PATH_SIZE];
    memcpy(temporary, path, length + 1);
    if (length > 1 && temporary[length - 1] == '/') {
        temporary[length - 1] = '\0';
    }

    for (char *cursor = temporary + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(temporary, 0700) != 0 && errno != EEXIST) {
            return -1;
        }
        *cursor = '/';
    }
    return mkdir(temporary, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static inline int validate_relative_path(const char *path, int allow_root) {
    if (path[0] == '\0') {
        return allow_root ? 0 : -1;
    }
    if (path[0] == '/' || path[0] == '~') {
        return -1;
    }

    const char *segment = path;
    while (*segment != '\0') {
        const char *slash = strchr(segment, '/');
        size_t length = slash == NULL ? strlen(segment) : (size_t)(slash - segment);
        if (length == 0 ||
            (length == 1 && segment[0] == '.') ||
            (length == 2 && segment[0] == '.' && segment[1] == '.') ||
            memchr(segment, '\\', length) != NULL) {
            return -1;
        }
        if (slash == NULL) {
            break;
        }
        segment = slash + 1;
    }
    return 0;
}

static inline int is_safe_filename(const char *filename) {
    return filename[0] != '\0' && strchr(filename, '/') == NULL &&
           strchr(filename, '\\') == NULL && strcmp(filename, ".") != 0 &&
           strcmp(filename, "..") != 0;
}

static inline int build_rooted_path(const char *root, const char *relative_path,
                                    int allow_root, char *output,
                                    size_t output_size) {
    if (validate_relative_path(relative_path, allow_root) < 0) {
        return -1;
    }
    int written = relative_path[0] == '\0'
                      ? snprintf(output, output_size, "%s", root)
                      : snprintf(output, output_size, "%s/%s", root, relative_path);
    return written >= 0 && (size_t)written < output_size ? 0 : -1;
}


/* Client paths use a virtual root, independently of each node's disk location. */
static inline int get_relative_virtual_path(const char *path,
                                            const char **relative,
                                            int allow_root) {
    if (path[0] != '/') return -1;
    if (validate_relative_path(path + 1, allow_root) < 0) return -1;
    *relative = path + 1;
    return 0;
}

static inline int build_data_directory(const char *node_name, char *output,
                                      size_t output_size) {
    const char *data_directory = getenv("FILEMESH_DATA_DIR");
    int written;
    if (data_directory != NULL && data_directory[0] != '\0') {
        if (data_directory[0] != '/') {
            fprintf(stderr, "Error: FILEMESH_DATA_DIR must be an absolute path\n");
            return -1;
        }
        written = snprintf(output, output_size, "%s/%s", data_directory, node_name);
    } else {
        const char *home_directory = getenv("HOME");
        if (home_directory == NULL || home_directory[0] != '/') {
            fprintf(stderr, "Error: Set HOME or FILEMESH_DATA_DIR to an absolute path\n");
            return -1;
        }
        written = snprintf(output, output_size, "%s/.local/share/filemesh/%s",
                           home_directory, node_name);
    }
    return written >= 0 && (size_t)written < output_size ? 0 : -1;
}

/* Use relative, NUL-delimited names so archives preserve folders safely. */
static inline int write_archive_entries(const char *root, const char *relative,
                                       const char *extension, FILE *list) {
    char directory[FILEMESH_PATH_SIZE];
    if (build_rooted_path(root, relative, 1, directory, sizeof(directory)) < 0)
        return -1;
    DIR *dir = opendir(directory);
    if (dir == NULL) return -1;

    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char entry_relative[FILEMESH_PATH_SIZE];
        char full_path[FILEMESH_PATH_SIZE];
        int written = relative[0] == '\0'
                          ? snprintf(entry_relative, sizeof(entry_relative), "%s",
                                     entry->d_name)
                          : snprintf(entry_relative, sizeof(entry_relative), "%s/%s",
                                     relative, entry->d_name);
        struct stat file_status;
        if (written < 0 || (size_t)written >= sizeof(entry_relative) ||
            build_rooted_path(root, entry_relative, 0, full_path, sizeof(full_path)) < 0 ||
            lstat(full_path, &file_status) < 0) {
            result = -1;
            break;
        }
        if (S_ISDIR(file_status.st_mode)) {
            if (write_archive_entries(root, entry_relative, extension, list) < 0) {
                result = -1;
                break;
            }
        } else if (S_ISREG(file_status.st_mode)) {
            const char *entry_extension = strrchr(entry->d_name, '.');
            if (entry_extension != NULL && strcmp(entry_extension, extension) == 0) {
                size_t length = strlen(entry_relative) + 1;
                if (fwrite(entry_relative, 1, length, list) != length) {
                    result = -1;
                    break;
                }
            }
        }
    }
    closedir(dir);
    return result;
}

/* No shell interpolation: storage paths and filenames are passed as arguments. */
static inline int send_archive(int sockfd, const char *root,
                               const char *extension) {
    char archive_path[] = "/tmp/filemesh-archive-XXXXXX";
    char list_path[] = "/tmp/filemesh-list-XXXXXX";
    int archive_fd = mkstemp(archive_path);
    if (archive_fd < 0) return send_int(sockfd, -1);
    close(archive_fd);
    int list_fd = mkstemp(list_path);
    if (list_fd < 0) {
        unlink(archive_path);
        return send_int(sockfd, -1);
    }
    FILE *list = fdopen(list_fd, "wb");
    if (list == NULL) {
        close(list_fd);
        unlink(list_path);
        unlink(archive_path);
        return send_int(sockfd, -1);
    }
    int result = write_archive_entries(root, "", extension, list);
    if (fclose(list) != 0) result = -1;
    if (result == 0) {
        pid_t child = fork();
        if (child == 0) {
            execlp("tar", "tar", "--create", "--file", archive_path,
                   "--directory", root, "--null", "--verbatim-files-from",
                   "--files-from", list_path, (char *)NULL);
            _exit(127);
        }
        if (child < 0) {
            result = -1;
        } else {
            int status;
            pid_t waited;
            do {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
                result = -1;
        }
    }
    int sent = result == 0 ? send_file_data(sockfd, archive_path)
                           : send_int(sockfd, -1);
    unlink(list_path);
    unlink(archive_path);
    return sent;
}

static inline int compare_names(const void *left, const void *right) {
    const char *const *left_name = left;
    const char *const *right_name = right;
    return strcmp(*left_name, *right_name);
}

static inline int write_sorted_filenames(const char *directory,
                                         const char *extension, FILE *output) {
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        const char *entry_extension = strrchr(entry->d_name, '.');
        if (entry_extension == NULL || strcmp(entry_extension, extension) != 0) {
            continue;
        }

        char full_path[FILEMESH_PATH_SIZE];
        int written = snprintf(full_path, sizeof(full_path), "%s/%s", directory,
                               entry->d_name);
        struct stat file_status;
        if (written < 0 || (size_t)written >= sizeof(full_path) ||
            stat(full_path, &file_status) < 0 || !S_ISREG(file_status.st_mode)) {
            continue;
        }

        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
            char **resized = realloc(names, new_capacity * sizeof(*names));
            if (resized == NULL) {
                closedir(dir);
                for (size_t index = 0; index < count; index++) free(names[index]);
                free(names);
                return -1;
            }
            names = resized;
            capacity = new_capacity;
        }

        size_t name_length = strlen(entry->d_name) + 1;
        names[count] = malloc(name_length);
        if (names[count] == NULL) {
            closedir(dir);
            for (size_t index = 0; index < count; index++) free(names[index]);
            free(names);
            return -1;
        }
        memcpy(names[count], entry->d_name, name_length);
        count++;
    }
    closedir(dir);

    if (count > 1) {
        qsort(names, count, sizeof(*names), compare_names);
    }
    int result = 0;
    for (size_t index = 0; index < count; index++) {
        if (fprintf(output, "%s\n", names[index]) < 0) result = -1;
        free(names[index]);
    }
    free(names);
    return result;
}

typedef void (*ClientProcessor)(int client_sockfd);

static inline void reap_children(int signal_number) {
    int saved_errno = errno;
    (void)signal_number;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = saved_errno;
}

static inline int install_server_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = reap_children;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, NULL) < 0) {
        return -1;
    }
    return signal(SIGPIPE, SIG_IGN) == SIG_ERR ? -1 : 0;
}

static inline void serve_connections(int sockfd, int backlog, const char *server_name,
                                     const char *peer_name,
                                     ClientProcessor process_client) {
    if (listen(sockfd, backlog) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    printf("%s listening on loopback\n", server_name);

    while (1) {
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);
        int client_sockfd = accept(sockfd, (struct sockaddr *)&client_address,
                                   &client_length);
        if (client_sockfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        printf("%s connected from %s:%d\n", peer_name,
               inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));

        fflush(NULL);
        pid_t child = fork();
        if (child == 0) {
            /* Request handlers may fork tar and must be able to wait for it. */
            signal(SIGCHLD, SIG_DFL);
            close(sockfd);
            process_client(client_sockfd);
            close(client_sockfd);
            exit(EXIT_SUCCESS);
        }
        if (child > 0) {
            close(client_sockfd);
        } else {
            perror("fork");
            close(client_sockfd);
        }
    }
}

#ifdef FILEMESH_ENABLE_STORAGE_SERVER

typedef struct StorageServerConfig {
    const char *server_name;
    const char *directory_name;
    const char *extension;
    const char *type_label;
    int port;
    int backlog;
} StorageServerConfig;

static StorageServerConfig storage_config;
static char storage_root[FILEMESH_PATH_SIZE];

static inline int storage_has_expected_extension(const char *path) {
    const char *extension = strrchr(path, '.');
    return extension != NULL && strcmp(extension, storage_config.extension) == 0;
}

static inline int storage_build_path(const char *relative_path, int allow_root,
                                     char *output, size_t output_size) {
    return build_rooted_path(storage_root, relative_path, allow_root, output,
                             output_size);
}

static inline int storage_handle_upload(int client_sockfd) {
    char relative_path[FILEMESH_PATH_SIZE];
    char filepath[FILEMESH_PATH_SIZE];
    int file_size;

    if (recv_string(client_sockfd, relative_path, sizeof(relative_path)) < 0 ||
        !storage_has_expected_extension(relative_path) ||
        storage_build_path(relative_path, 0, filepath, sizeof(filepath)) < 0) {
        send_int(client_sockfd, -1);
        return -1;
    }

    char directory[FILEMESH_PATH_SIZE];
    snprintf(directory, sizeof(directory), "%s", filepath);
    char *last_slash = strrchr(directory, '/');
    if (last_slash != NULL) {
        *last_slash = '\0';
        if (create_directory(directory) < 0) {
            send_int(client_sockfd, -1);
            return -1;
        }
    }

    if (recv_int(client_sockfd, &file_size) < 0 || file_size < 0 ||
        recv_file_data(client_sockfd, filepath, file_size) < 0) {
        send_int(client_sockfd, -1);
        return -1;
    }

    printf("%s uploaded: %s\n", storage_config.type_label, filepath);
    return send_int(client_sockfd, 0);
}

static inline int storage_handle_download(int client_sockfd) {
    char relative_path[FILEMESH_PATH_SIZE];
    char filepath[FILEMESH_PATH_SIZE];
    if (recv_string(client_sockfd, relative_path, sizeof(relative_path)) < 0 ||
        !storage_has_expected_extension(relative_path) ||
        storage_build_path(relative_path, 0, filepath, sizeof(filepath)) < 0) {
        send_int(client_sockfd, -1);
        return -1;
    }
    return send_file_data(client_sockfd, filepath);
}

static inline int storage_handle_remove(int client_sockfd) {
    char relative_path[FILEMESH_PATH_SIZE];
    char filepath[FILEMESH_PATH_SIZE];
    if (recv_string(client_sockfd, relative_path, sizeof(relative_path)) < 0 ||
        !storage_has_expected_extension(relative_path) ||
        storage_build_path(relative_path, 0, filepath, sizeof(filepath)) < 0) {
        send_int(client_sockfd, -1);
        return -1;
    }

    int status = unlink(filepath) == 0 ? 0 : -1;
    if (send_int(client_sockfd, status) < 0) return -1;
    return status;
}

static inline int storage_handle_tar(int client_sockfd) {
    return send_archive(client_sockfd, storage_root, storage_config.extension);
}

static inline int storage_handle_display(int client_sockfd) {
    char relative_path[FILEMESH_PATH_SIZE];
    char directory[FILEMESH_PATH_SIZE];
    if (recv_string(client_sockfd, relative_path, sizeof(relative_path)) < 0 ||
        storage_build_path(relative_path, 1, directory, sizeof(directory)) < 0) {
        send_int(client_sockfd, -1);
        return -1;
    }

    FILE *output = tmpfile();
    if (output == NULL ||
        write_sorted_filenames(directory, storage_config.extension, output) < 0 ||
        fflush(output) != 0 || fseek(output, 0, SEEK_END) != 0) {
        if (output != NULL) fclose(output);
        send_int(client_sockfd, -1);
        return -1;
    }

    long list_size = ftell(output);
    if (list_size < 0 || list_size > INT_MAX || fseek(output, 0, SEEK_SET) != 0 ||
        send_int(client_sockfd, (int)list_size) < 0) {
        fclose(output);
        return -1;
    }

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), output)) > 0) {
        if (send_bytes(client_sockfd, buffer, bytes_read) < 0) {
            fclose(output);
            return -1;
        }
    }
    fclose(output);
    return 0;
}

static inline void storage_process_client(int client_sockfd) {
    int operation;
    while (recv_int(client_sockfd, &operation) == 0) {
        int result;
        switch (operation) {
            case OP_UPLOAD: result = storage_handle_upload(client_sockfd); break;
            case OP_DOWNLOAD: result = storage_handle_download(client_sockfd); break;
            case OP_REMOVE: result = storage_handle_remove(client_sockfd); break;
            case OP_TAR: result = storage_handle_tar(client_sockfd); break;
            case OP_DISPLAY: result = storage_handle_display(client_sockfd); break;
            default: return;
        }
        if (result < 0) return;
    }
}

static inline int run_storage_server(const StorageServerConfig *config) {
    storage_config = *config;
    if (install_server_signal_handlers() < 0) {
        perror("signal");
        return EXIT_FAILURE;
    }

    if (build_data_directory(storage_config.directory_name, storage_root,
                             sizeof(storage_root)) < 0 ||
        create_directory(storage_root) < 0) {
        fprintf(stderr, "Error: Cannot create %s storage directory\n",
                storage_config.server_name);
        return EXIT_FAILURE;
    }

    int sockfd = initialize_socket(storage_config.port);
    if (sockfd < 0) return EXIT_FAILURE;
    serve_connections(sockfd, storage_config.backlog, storage_config.server_name,
                      "Gateway", storage_process_client);
    close(sockfd);
    return EXIT_SUCCESS;
}

#endif /* FILEMESH_ENABLE_STORAGE_SERVER */
#endif /* SERVER_UTILS_H */
