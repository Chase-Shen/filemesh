#include "server_utils.h"

#define BACKLOG 16
#define MAX_PATH_LEN FILEMESH_PATH_SIZE
#define MAX_FILES FILEMESH_MAX_BATCH_FILES

static char storage_dir[MAX_PATH_LEN];

// Get file extension
static const char *get_file_extension(const char *filename) {
    const char *dot_ptr = strrchr(filename, '.');
    if (dot_ptr == NULL || dot_ptr == filename) 
        return "";
    return dot_ptr;
}

// Determine which server should handle the file based on extension
static int get_port_by_ext(const char *filename) {
    const char *ext = get_file_extension(filename);
    if (strcmp(ext, ".pdf") == 0) {
        return FILEMESH_PDF_PORT;
    }
    if (strcmp(ext, ".txt") == 0) {
        return FILEMESH_TEXT_PORT;
    }
    if (strcmp(ext, ".zip") == 0) {
        return FILEMESH_ZIP_PORT;
    }
    if (strcmp(ext, ".c") == 0) {
        return 0; // gateway handles .c files locally
    }
    return -1; // Invalid file type
}

static int build_storage_path(const char *virtual_path, int allow_root,
                              char *output, size_t output_size) {
    const char *relative;
    if (get_relative_virtual_path(virtual_path, &relative, allow_root) < 0) {
        return -1;
    }

    return build_rooted_path(storage_dir, relative, allow_root, output, output_size);
}

// Forward file to appropriate server
static int forward_file_to_server(const char *filename, const char *filepath, int server_port) {
    int server_sockfd = connect_to_server(FILEMESH_HOST, server_port);
    if (server_sockfd < 0) {
        printf("Error: Failed to connect to server on port %d...\n", server_port);
        return -1;
    }

    // Send upload operation
    if (send_int(server_sockfd, OP_UPLOAD) < 0) {
        printf("Error: Failed to send upload operation to server on port %d...\n", server_port);
        close(server_sockfd);
        return -1;
    }

    // Send filename
    if (send_string(server_sockfd, filepath) < 0) {
        printf("Error: Failed to send filepath '%s' to server on port %d...\n", filepath, server_port);
        close(server_sockfd);
        return -1;
    }

    // Send file data
    int total_sent = send_file_data(server_sockfd, filename);
    if (total_sent < 0) {
        printf("Error: Failed to send file data for '%s' to server on port %d...\n", filepath, server_port);
        close(server_sockfd);
        return -1;
    }

    printf("File '%s' forwarded to server on port %d successfully.\n", filepath, server_port);

    // Receive status from server
    int status;
    if (recv_int(server_sockfd, &status) < 0 || status != 0) {
        printf("Error: Upload failed for '%s' on server port %d...\n", filename, server_port);
        close(server_sockfd);
        return -1;
    }

    close(server_sockfd);
    return 0;
}

// Handle upload request
static int handle_upload_request(int client_sockfd) {
    int num_files;
    char destination_path[MAX_PATH_LEN];
    
    // Receive number of files
    if (recv_int(client_sockfd, &num_files) < 0 || num_files <= 0 || num_files > MAX_FILES) {
        printf("Error: Invalid number of files...\n");
        return -1;
    }
    
    // Receive destination path
    if (recv_string(client_sockfd, destination_path, sizeof(destination_path)) < 0) {
        printf("Error: Failed to receive destination path...\n");
        return -1;
    }

    size_t destination_length = strlen(destination_path);
    if (destination_length > 1 && destination_path[destination_length - 1] == '/') {
        destination_path[destination_length - 1] = '\0';
    }

    const char *relative_destination;
    if (get_relative_virtual_path(destination_path, &relative_destination, 1) < 0) {
        printf("Error: Invalid destination path '%s'...\n", destination_path);
        return -1;
    }
    
    printf("Upload request from client: %d files to %s\n", num_files, destination_path);
    
    // Process each file
    for (int i = 0; i < num_files; i++) {
        char filename[MAX_PATH_LEN];
        
        
        // Receive filename
        if (recv_string(client_sockfd, filename, sizeof(filename)) < 0) {
            printf("Error: Failed to receive filename...\n");
            return -1;
        }
        
        if (!is_safe_filename(filename) || get_port_by_ext(filename) < 0) {
            printf("Error: Invalid filename '%s'...\n", filename);
            return -1;
        }

        int file_size = recv_file_size(client_sockfd, filename);
        if (file_size < 0) {
            return -1;
        }
        
        printf("Receiving file: %s (%d bytes)\n", filename, file_size);
        
        // Create temporary file to store received file
        char temp_filepath[MAX_PATH_LEN];
        int written = snprintf(temp_filepath, sizeof(temp_filepath), "%s/.upload_%ld_%d",
                               storage_dir, (long)getpid(), i);
        if (written < 0 || (size_t)written >= sizeof(temp_filepath)) {
            printf("Error: Temporary path is too long...\n");
            return -1;
        }
        
        // Receive file data
        int total_received = recv_file_data(client_sockfd, temp_filepath, file_size);
        if (total_received < 0) {
            return -1;
        }
        
        // Determine destination based on file type
        int server_port = get_port_by_ext(filename);

        if (server_port > 0) {
            // Forward to appropriate server
            char server_path[MAX_PATH_LEN];
            written = relative_destination[0] == '\0'
                        ? snprintf(server_path, sizeof(server_path), "%s", filename)
                        : snprintf(server_path, sizeof(server_path), "%s/%s",
                                   relative_destination, filename);
            if (written < 0 || (size_t)written >= sizeof(server_path)) {
                unlink(temp_filepath);
                printf("Error: Destination path is too long...\n");
                return -1;
            }
            
            if (forward_file_to_server(temp_filepath, server_path, server_port) < 0) {
                unlink(temp_filepath);
                printf("Error: Failed to forward file '%s' to server on port %d\n", filename, server_port);
                return -1;
            }
            
            // Remove temporary file
            unlink(temp_filepath);
            printf("Forwarded file to server on port %d: %s\n", server_port, filename);        
        } else {
            char destination_dir[MAX_PATH_LEN];
            char local_path[MAX_PATH_LEN];

            written = relative_destination[0] == '\0'
                        ? snprintf(destination_dir, sizeof(destination_dir), "%s", storage_dir)
                        : snprintf(destination_dir, sizeof(destination_dir), "%s/%s",
                                   storage_dir, relative_destination);
            if (written < 0 || (size_t)written >= sizeof(destination_dir) ||
                create_directory(destination_dir) < 0) {
                unlink(temp_filepath);
                printf("Error: Failed to create destination directory...\n");
                return -1;
            }

            written = snprintf(local_path, sizeof(local_path), "%s/%s",
                               destination_dir, filename);
            if (written < 0 || (size_t)written >= sizeof(local_path) ||
                rename(temp_filepath, local_path) < 0) {
                unlink(temp_filepath);
                printf("Error: Failed to store '%s' in '%s'...\n", filename, destination_dir);
                return -1;
            }
            printf("Stored .c file locally: %s\n", local_path);
        }
    }
    return send_int(client_sockfd, 0);
}

// Stream a file from a storage server to the client without buffering it in memory.
static int forward_file_from_server(int client_sockfd, const char *filepath, int server_port) {
    int server_sockfd = connect_to_server(FILEMESH_HOST, server_port);
    if (server_sockfd < 0) {
        return send_int(client_sockfd, -1);
    }

    // Send download operation
    if (send_int(server_sockfd, OP_DOWNLOAD) < 0) {
        close(server_sockfd);
        return send_int(client_sockfd, -1);
    }

    // Send filepath
    if (send_string(server_sockfd, filepath) < 0) {
        close(server_sockfd);
        return send_int(client_sockfd, -1);
    }

    int file_size;
    if (recv_int(server_sockfd, &file_size) < 0) {
        close(server_sockfd);
        return send_int(client_sockfd, -1);
    }
    if (send_int(client_sockfd, file_size) < 0) {
        close(server_sockfd);
        return -1;
    }
    if (file_size < 0) {
        close(server_sockfd);
        return 0;
    }

    char buffer[BUFFER_SIZE];
    int bytes_remaining = file_size;
    while (bytes_remaining > 0) {
        size_t chunk_size = bytes_remaining < BUFFER_SIZE
                                ? (size_t)bytes_remaining
                                : (size_t)BUFFER_SIZE;
        ssize_t bytes_received = recv_bytes(server_sockfd, buffer, chunk_size);
        if (bytes_received <= 0 ||
            send_bytes(client_sockfd, buffer, (size_t)bytes_received) < 0) {
            close(server_sockfd);
            return -1;
        }
        bytes_remaining -= (int)bytes_received;
    }

    close(server_sockfd);
    return 0;
}

// Handle downloads
static int handle_download_request(int client_sockfd) {
    int num_files;
    
    // Receive number of files
    if (recv_int(client_sockfd, &num_files) < 0 || num_files <= 0 || num_files > MAX_FILES) {
        send_int(client_sockfd, -1); // Send failure
        return -1;
    }

    printf("Download request from client: %d files\n", num_files);

    // Process each file
    for (int i = 0; i < num_files; i++) {
        char filepath[MAX_PATH_LEN];
        
        // Receive filepath
        if (recv_string(client_sockfd, filepath, sizeof(filepath)) < 0) {
            printf("Error: Failed to receive filepath...\n");
            send_int(client_sockfd, 0); // Send failure
            return -1;
        }
        
        printf("Download request for: %s\n", filepath);
        
        const char *relative_path;
        if (get_relative_virtual_path(filepath, &relative_path, 0) < 0) {
            printf("Error: Invalid storage path '%s'\n", filepath);
            if (send_int(client_sockfd, -1) < 0) {
                return -1;
            }
            continue;
        }

        const char *filename = strrchr(relative_path, '/');
        filename = filename == NULL ? relative_path : filename + 1;

        int server_port = get_port_by_ext(filename);
        
        if (server_port == 0) {
            // .c file - read locally from gateway
            char local_path[MAX_PATH_LEN];
            if (build_storage_path(filepath, 0, local_path, sizeof(local_path)) < 0) {
                send_int(client_sockfd, -1);
                continue;
            }
            if (send_file_data(client_sockfd, local_path) == 0) {
                printf("Sent .c file from local storage: %s\n", local_path);
            }
            
        } else if (server_port > 0) {
            if (forward_file_from_server(client_sockfd, relative_path, server_port) < 0) {
                printf("Error: Failed to forward '%s' from server port %d\n",
                       relative_path, server_port);
                return -1;
            }
            printf("Retrieved file from server on port %d: %s\n", server_port, filename);

        } else {
            // Invalid file type
            send_int(client_sockfd, -1); // Send file size -1 (not found)
            printf("Error: Invalid file type for '%s'\n", filename);
        }
    }
    return 0;
}

// Handle removals
static int handle_remove_request(int client_sockfd) {
    int num_files;
    // Receive number of files
    if (recv_int(client_sockfd, &num_files) < 0 || num_files <= 0 || num_files > MAX_FILES) {
        send_int(client_sockfd, -1); // Send failure
        return -1;
    }

    printf("Remove request from client: %d files\n", num_files);

    int success_count = 0;

    // Process each file
    for (int i = 0; i < num_files; i++) {
        char filepath[MAX_PATH_LEN];
        
        // Receive filepath
        if (recv_string(client_sockfd, filepath, sizeof(filepath)) < 0) {
            send_int(client_sockfd, -1); // Send failure
            return -1;
        }
        
        printf("Remove request for: %s\n", filepath);
        
        const char *relative_path;
        if (get_relative_virtual_path(filepath, &relative_path, 0) < 0) {
            printf("Error: Invalid gateway path '%s'\n", filepath);
            continue;
        }

        const char *filename = strrchr(relative_path, '/');
        filename = filename == NULL ? relative_path : filename + 1;

        int server_port = get_port_by_ext(filename);

        if (server_port == 0) {
            // .c file - remove locally from gateway
            char local_path[MAX_PATH_LEN];
            if (build_storage_path(filepath, 0, local_path, sizeof(local_path)) < 0) {
                continue;
            }
            
            if (unlink(local_path) == 0) {
                success_count++;
                printf("Successfully removed .c file from local storage: %s\n", local_path);
            } else {
                printf("Error: Failed to remove .c file: %s\n", local_path);
            }
            
        } else if (server_port > 0) {
            // Request removal from appropriate server
            int server_sockfd = connect_to_server(FILEMESH_HOST, server_port);
            if (server_sockfd < 0) {
                continue;
            }
            
            // Send remove operation
            if (send_int(server_sockfd, OP_REMOVE) < 0) {
                printf("Error: Failed to send remove operation to server on port %d...\n", server_port);
                close(server_sockfd);
                continue;
            }
            
            // Send filepath
            if (send_string(server_sockfd, relative_path) < 0) {
                printf("Error: Failed to send filepath '%s' to server on port %d...\n", relative_path, server_port);
                close(server_sockfd);
                continue;
            }
            
            // Receive status
            int status;
            if (recv_int(server_sockfd, &status) < 0 || status != 0) {
                printf("Error: Failed to remove file from server on port %d: %s\n", server_port, filename);
            } else {
                success_count++;
                printf("Successfully removed file from server on port %d: %s\n", server_port, filename);
            }
            close(server_sockfd); 
        }
    }
    
    // Send overall status
    send_int(client_sockfd, success_count);
    return 0;
}

// Handle archive downloads
static int handle_tar_request(int client_sockfd) {
    char filetype[16];
    
    // Receive filetype
    if (recv_string(client_sockfd, filetype, sizeof(filetype)) < 0) {
        send_int(client_sockfd, -1); // Send failure
        return -1;
    }
    
    printf("Tar request for filetype: %s\n", filetype);
    
    if (strcmp(filetype, ".c") == 0) {
        return send_archive(client_sockfd, storage_dir, ".c");
    } else {
        // Request tar from appropriate server
        int server_port = -1;
        if (strcmp(filetype, ".pdf") == 0) {
            server_port = FILEMESH_PDF_PORT;
        } else if (strcmp(filetype, ".txt") == 0) {
            server_port = FILEMESH_TEXT_PORT;
        } else if (strcmp(filetype, ".zip") == 0) {
            server_port = FILEMESH_ZIP_PORT;
        }

        if (server_port == -1) {
            send_int(client_sockfd, -1);
            return -1;
        }
        
        int server_sockfd = connect_to_server(FILEMESH_HOST, server_port);
        if (server_sockfd < 0) {
            send_int(client_sockfd, -1);
            return -1;
        }
        
        // Send tar operation
        if (send_int(server_sockfd, OP_TAR) < 0) {
            close(server_sockfd);
            send_int(client_sockfd, -1);
            return -1;
        }
        
        // Receive file size from server
        int file_size;
        if (recv_int(server_sockfd, &file_size) < 0) {
            close(server_sockfd);
            send_int(client_sockfd, -1);
            return -1;
        }
        
        // Forward file size to client
        if (send_int(client_sockfd, file_size) < 0) {
            close(server_sockfd);
            return -1;
        }
        
        if (file_size > 0) {
            // Forward file data
            char buffer[BUFFER_SIZE];
            int bytes_remaining = file_size;
            while (bytes_remaining > 0) {
                int to_receive = (bytes_remaining < BUFFER_SIZE) ? bytes_remaining : BUFFER_SIZE;
                ssize_t bytes_received = recv_bytes(server_sockfd, buffer, to_receive);
                if (bytes_received <= 0) {
                    close(server_sockfd);
                    return -1;
                }
                
                if (send_bytes(client_sockfd, buffer, bytes_received) < 0) {
                    close(server_sockfd);
                    return -1;
                }
                bytes_remaining -= bytes_received;
            }
        }
        
        close(server_sockfd);
        printf("Forwarded tar file from server on port %d\n", server_port);
    }
    
    return 0;
}

// Handle directory listings
static int handle_display_request(int client_sockfd) {
    char pathname[MAX_PATH_LEN];
    const char *relative_path;
    
    // Receive pathname
    if (recv_string(client_sockfd, pathname, sizeof(pathname)) < 0) {
        send_int(client_sockfd, 0); // Send failure
        return -1;
    }
    
    printf("Display request from client for path: %s\n", pathname);
    
    char local_path[MAX_PATH_LEN];
    if (get_relative_virtual_path(pathname, &relative_path, 1) < 0 ||
        build_storage_path(pathname, 1, local_path, sizeof(local_path)) < 0) {
        send_int(client_sockfd, -1);
        return -1;
    }

    // An anonymous temporary stream avoids collisions between forked clients.
    FILE *file = tmpfile();
    if (file == NULL) {
        send_int(client_sockfd, -1); // Send failure
        return -1;
    }
    
    // Group filenames by type, sorting each group's names.
    if (write_sorted_filenames(local_path, ".c", file) < 0) {
        fclose(file);
        send_int(client_sockfd, -1);
        return -1;
    }
    
    // Request filenames from PDF, text, and ZIP servers
    // Order: PDF (.pdf), text (.txt), ZIP (.zip) to maintain file type ordering
    int servers[] = {FILEMESH_PDF_PORT, FILEMESH_TEXT_PORT, FILEMESH_ZIP_PORT};
    const char* server_names[] = {"PDF", "text", "ZIP"};
    const char* file_types[] = {".pdf", ".txt", ".zip"};

    for (int i = 0; i < 3; i++) {
        printf("Trying to connect to %s (port %d) for %s files...\n", 
               server_names[i], servers[i], file_types[i]);
        
        int server_sockfd = connect_to_server(FILEMESH_HOST, servers[i]);
        if (server_sockfd < 0) {
            printf("Warning: %s server not available\n", server_names[i]);
            continue;
        }
        
        printf("Connected to %s server\n", server_names[i]);
        
        // Send display operation
        if (send_int(server_sockfd, OP_DISPLAY) < 0) {
            printf("Error: Failed to send OP_DISPLAY to %s\n", server_names[i]);
            close(server_sockfd);
            continue;
        }
        
        // Send pathname
        if (send_string(server_sockfd, relative_path) < 0) {
            printf("Error: Failed to send pathname to %s\n", server_names[i]);
            close(server_sockfd);
            continue;
        }
        
        // Receive list size
        int list_size;
        if (recv_int(server_sockfd, &list_size) < 0) {
            printf("Error: Failed to receive list size from %s\n", server_names[i]);
            close(server_sockfd);
            continue;
        }
        
        printf("Received list size from %s: %d bytes\n", server_names[i], list_size);
        
        if (list_size <= 0) {
            printf("%s has no %s files\n", server_names[i], file_types[i]);
            close(server_sockfd);
            continue;
        }
        
        // Stream the response into the consolidated list.
        char buffer[BUFFER_SIZE];
        int bytes_remaining = list_size;
        int receive_failed = 0;
        while (bytes_remaining > 0) {
            size_t chunk_size = bytes_remaining < BUFFER_SIZE
                                    ? (size_t)bytes_remaining
                                    : (size_t)BUFFER_SIZE;
            ssize_t bytes_received = recv_bytes(server_sockfd, buffer, chunk_size);
            if (bytes_received <= 0 ||
                fwrite(buffer, 1, (size_t)bytes_received, file) != (size_t)bytes_received) {
                receive_failed = 1;
                break;
            }
            bytes_remaining -= (int)bytes_received;
        }
        close(server_sockfd);
        if (receive_failed) {
            fclose(file);
            send_int(client_sockfd, -1);
            return -1;
        }
        
        printf("Successfully processed %s files from %s\n", file_types[i], server_names[i]);
    }
    
    // Get file size
    if (fflush(file) != 0 || fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        send_int(client_sockfd, -1);
        return -1;
    }
    long list_size = ftell(file);
    if (list_size < 0 || list_size > INT_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        send_int(client_sockfd, -1);
        return -1;
    }
    
    printf("Total consolidated list size: %ld bytes\n", list_size);
    
    // Send list size
    if (send_int(client_sockfd, (int)list_size) < 0) {
        fclose(file);
        return -1;
    }
    
    if (list_size > 0) {
        // Send file list
        char buffer[BUFFER_SIZE];
        size_t bytes_read;
        while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
            if (send_bytes(client_sockfd, buffer, bytes_read) < 0) {
                fclose(file);
                return -1;
            }
        }
    }
    
    fclose(file);
    
    printf("Sent consolidated file list to client\n");
    return 0;
}

// Process client requests
static void process_client(int client_sockfd) {
    int operation;
    
    while (1) {
        // Receive operation from client
        if (recv_int(client_sockfd, &operation) < 0) {
            break;
        }
        int result;
        switch (operation) {
            case OP_UPLOAD:
                result = handle_upload_request(client_sockfd);
                break;
            case OP_DOWNLOAD:
                result = handle_download_request(client_sockfd);
                break;
            case OP_REMOVE:
                result = handle_remove_request(client_sockfd);
                break;
            case OP_TAR:
                result = handle_tar_request(client_sockfd);
                break;
            case OP_DISPLAY:
                result = handle_display_request(client_sockfd);
                break;
            default:
                printf("Error: Unknown operation: %d\n", operation);
                return;
        }
        if (result < 0) {
            break;
        }
    }
}

int main(void) {
    if (install_server_signal_handlers() < 0) {
        perror("signal");
        return EXIT_FAILURE;
    }

    if (build_data_directory("source", storage_dir, sizeof(storage_dir)) < 0) {
        fprintf(stderr, "Error: Cannot determine gateway storage directory\n");
        return EXIT_FAILURE;
    }

    // Create base directory if it doesn't exist
    if (create_directory(storage_dir) != 0) {
        printf("Error: Fail to create directory: %s\n", storage_dir);
        return EXIT_FAILURE;
    }
    
    int sockfd = initialize_socket(FILEMESH_GATEWAY_PORT);
    if (sockfd < 0) {
        return EXIT_FAILURE;
    }
    serve_connections(sockfd, BACKLOG, "Gateway", "Client", process_client);
    close(sockfd);
    return 0;
}
