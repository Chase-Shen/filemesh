#include "server_utils.h"

#define MAX_ARGS (FILEMESH_MAX_BATCH_FILES + 2)

// Helper function to send operation code, number of files, and file paths
static int send_op_filenum_path(int sockfd, int operation, int num_files, char **filepaths) {

    // Send operation code
    if (send_int(sockfd, operation) < 0) {
        printf("Error: Failed to send operation code '%d'\n", operation);
        return -1;
    }

    // Send number of files
    if (send_int(sockfd, num_files) < 0) {
        printf("Error: Failed to send number of files '%d'\n", num_files);
        return -1;
    }

    // Send each file path
    for (int i = 0; i < num_files; i++) {
        if (send_string(sockfd, filepaths[i]) < 0) {
            printf("Error: Failed to send file path '%s'\n", filepaths[i]);
            return -1;
        }
    }
    return 0;
}
    
// Validate file extension
static int validate_file_extension(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (ext == NULL) 
        return -1;

    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".pdf") == 0 || strcmp(ext, ".txt") == 0 || strcmp(ext, ".zip") == 0) {
        return 0;
    }
    return -1;
}

// Check if file exists
static int file_exists(const char *filename) {
    struct stat file_stat;
    return stat(filename, &file_stat) == 0 && S_ISREG(file_stat.st_mode) ? 0 : -1;
}

static int validate_virtual_path(const char *path) {
    const char *relative;
    return get_relative_virtual_path(path, &relative, 1);
}

static const char *get_local_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

/* Local paths are allowed; only their safe basenames are stored remotely. */
static int validate_local_path(const char *path) {
    return is_safe_filename(get_local_basename(path)) &&
           strchr(path, '\\') == NULL ? 0 : -1;
}

// Handle upload command
static int handle_upload_command(char **args, int argc) {
    if (argc < 3 || argc > MAX_ARGS) {
        printf("Error: Invalid argument numbers...\n");
        return -1;
    }

    char *dest_path = args[argc - 1];
    // Validate destination path
    if (validate_virtual_path(dest_path) == -1) {
        printf("Error: Destination path must start with /\n");
        return -1;
    }

    int files_num = argc - 2; // Last argument is destination path
    // Check files existence and correct extensions
    for (int i = 1; i <= files_num; i++) {
        if (validate_local_path(args[i]) < 0) {
            printf("Error: Invalid local file path\n");
            return -1;
        }
        if (file_exists(args[i]) == -1) {
            printf("Error: File '%s' does not exist\n", args[i]);
            return -1;
        }
        if (validate_file_extension(args[i]) == -1) {
            printf("Error: Invalid file extension...\n");
            return -1;
        }
    }
    
    int sockfd = connect_to_server(FILEMESH_HOST, FILEMESH_GATEWAY_PORT);
    if (sockfd < 0) {
        printf("Error: Fail to connect to server\n");
        return -1;
    }
    
    // Send operation code
    if (send_int(sockfd, OP_UPLOAD) < 0) {
        printf("Error: Failed to send operation code\n");
        close(sockfd);
        return -1;
    }
    
    // Send number of files
    if (send_int(sockfd, files_num) < 0) {
        printf("Error: Failed to send number of files\n");
        close(sockfd);
        return -1;
    }
    
    // Send destination path
    if (send_string(sockfd, dest_path) < 0) {
        printf("Error: Failed to send destination path\n");
        close(sockfd);
        return -1;
    }
    
    // Send each file
    for (int i = 1; i <= files_num; i++) {
        char *filename = args[i];
        
        // Store only the basename, not the client's local directory path.
        if (send_string(sockfd, get_local_basename(filename)) < 0) {
            printf("Error: Failed to send filename '%s'...\n", filename);
            close(sockfd);
            return -1;
        }
        
        if (send_file_data(sockfd, filename) < 0) {
            printf("Error: Failed to send file data for '%s'...\n", filename);
            close(sockfd);
            return -1;
        }
    }
    
    int status;
    if (recv_int(sockfd, &status) < 0 || status != 0) {
        printf("Error: Upload was not confirmed by the gateway\n");
        close(sockfd);
        return -1;
    }
    close(sockfd);
    printf("Uploaded %d file(s) to %s\n", files_num, dest_path);
    return 0;
}

// Handle download command
static int handle_download_command(char **args, int argc) {
    if (argc < 2 || argc > FILEMESH_MAX_BATCH_FILES + 1) { // A bounded batch of file paths
        printf("Error: Invalid download command...\n");
        return -1;
    }

    int files_num = argc - 1;
    
    // Validate file paths
    for (int i = 1; i <= files_num; i++) {
        if (validate_virtual_path(args[i]) == -1 || validate_file_extension(args[i]) == -1) {
            printf("Error: File path '%s' must start with /\n", args[i]);
            return -1;
        }
    }
    
    printf("Downloading %d files...\n", files_num);
    
    int sockfd = connect_to_server(FILEMESH_HOST, FILEMESH_GATEWAY_PORT);
    if (sockfd < 0) {
        printf("Error: Could not connect to server...\n");
        return -1;
    }
    
    if (send_op_filenum_path(sockfd, OP_DOWNLOAD, files_num, &args[1]) < 0) {
        close(sockfd);
        return -1;
    }
    
    int result = 0;

    // Receive files
    for (int i = 1; i <= files_num; i++) {
        char *filepath = args[i];
        char *filename = strrchr(filepath, '/'); // Get filename from path
        if (filename == NULL) {
            filename = filepath; // No path, use the whole string as filename
        } else {
            filename++; // Skip the '/'
        }

        int file_size;
        if (recv_int(sockfd, &file_size) < 0) {
            printf("Error: Failed to receive file size for '%s'\n", filename);
            close(sockfd);
            return -1;
        }
        if (file_size < 0) {
            printf("File '%s' not found on server\n", filename);
            result = -1;
            continue;
        }

        // Receive file data
        int total_received = recv_file_data(sockfd, filename, file_size);
        if (total_received < 0) {
            close(sockfd);
            return -1;
        }
        printf("Download Complete (%d bytes received)\n", total_received);
    }
    
    close(sockfd);
    return result;
}

// Handle remove command
static int handle_remove_command(char **args, int argc) {
    if (argc < 2 || argc > FILEMESH_MAX_BATCH_FILES + 1) { // A bounded batch of file paths
        printf("Error: Invalid number of arguments...\n");
        return -1;
    }

    int files_num = argc - 1;
    
    // Validate file paths
    for (int i = 1; i <= files_num; i++) {
        if (validate_virtual_path(args[i]) < 0 || validate_file_extension(args[i]) < 0) {
            printf("Error: File path '%s' must start with /\n", args[i]);
            return -1;
        }
    }
    
    printf("Removing %d files...\n", files_num);
    
    int sockfd = connect_to_server(FILEMESH_HOST, FILEMESH_GATEWAY_PORT);
    if (sockfd < 0) {
        printf("Error: Failed to connect to server...\n");
        return -1;
    }
    
    if (send_op_filenum_path(sockfd, OP_REMOVE, files_num, &args[1]) < 0) {
        close(sockfd);
        return -1;
    }
    
    
    // Receive status from server
    int status;
    if (recv_int(sockfd, &status) < 0) {
        printf("Error: Failed to receive remove status...\n");
        close(sockfd);
        return -1;
    }
    
    close(sockfd);
    
    if (status == files_num) {
        printf("Remove operation successful!\n");
        return 0;
    } else if (status == files_num - 1) {
        printf("Warning: One file could not be removed...\n");
        return 0;
    } else if (status == 0) {
        printf("Warning: No files could be removed...\n");
        return -1;
    } else {
        printf("Error: Failed to remove files...\n");
        return -1;
    }
}

// Validate filetype for archive command
static int validate_filetype(const char *filetype) {
    if (strcmp(filetype, ".c") == 0 || strcmp(filetype, ".pdf") == 0 || strcmp(filetype, ".txt") == 0 || strcmp(filetype, ".zip") == 0) {
        return 0; // valid filetype
    }
    return -1; // invalid filetype
}

// Handle archive command
static int handle_archive_command(char **args, int argc) {
    if (argc != 2) {
        printf("Error: archive requires exactly one filetype argument...\n");
        return -1;
    }

    char *filetype = args[1];
    
    // Validate filetype
    if (validate_filetype(filetype) == -1) {
        printf("Error: Invalid filetype '%s'...\n", filetype);
        return -1;
    }
    
    printf("Requesting tar file for filetype: %s...\n", filetype);
    
    int sockfd = connect_to_server(FILEMESH_HOST, FILEMESH_GATEWAY_PORT);
    if (sockfd < 0) {
        printf("Error: Failed to connect to server...\n");
        return -1;
    }
    
    // Send tar command
    if (send_int(sockfd, OP_TAR) < 0) {
        printf("Error: Failed to send tar command...\n");
        close(sockfd);
        return -1;
    }
    
    // Send filetype
    if (send_string(sockfd, filetype) < 0) {
        printf("Error: Failed to send filetype...\n");
        close(sockfd);
        return -1;
    }
    
    // Receive file size
    int file_size = recv_file_size(sockfd, filetype);
    if (file_size < 0) {
        close(sockfd);
        return -1;
    }
    
    // Create tar filename
    const char *tar_filename;
    if (strcmp(filetype, ".c") == 0) {
        tar_filename = "cfiles.tar";
    } else if (strcmp(filetype, ".pdf") == 0) {
        tar_filename = "pdf.tar";
    } else if (strcmp(filetype, ".txt") == 0) {
        tar_filename = "text.tar";
    } else {
        tar_filename = "zip.tar";
    }
    
    printf("Downloading tar file '%s' (%d bytes)... ", tar_filename, file_size);
    fflush(stdout);
    
    // Receive tar file data
    int total_received = recv_file_data(sockfd, tar_filename, file_size);
    close(sockfd);

    printf("Tar file received (%d bytes)\n", total_received);
    printf("Tar file saved as: %s\n", tar_filename);
    
    return 0;
}

// Handle list command
static int handle_list_command(char **args, int argc) {
    if (argc != 2) {
        printf("Error: Invalid number of arguments for list...\n");
        return -1;
    }

    char *pathname = args[1];
    
    // Validate pathname
    if (validate_virtual_path(pathname) == -1) {
        printf("Error: Pathname '%s' must start with /\n", pathname);
        return -1;
    }
    
    printf("Requesting file list for directory: %s\n", pathname);
    
    // Connect to the gateway
    int sockfd = connect_to_server(FILEMESH_HOST, FILEMESH_GATEWAY_PORT);
    if (sockfd < 0) {
        printf("Error: Failed to connect to server...\n");
        return -1;
    }
    
    // Send display command
    if (send_int(sockfd, OP_DISPLAY) < 0) {
        printf("Error: Failed to send display command...\n");
        close(sockfd);
        return -1;
    }
    
    // Send pathname
    if (send_string(sockfd, pathname) < 0) {
        printf("Error: Failed to send pathname...\n");
        close(sockfd);
        return -1;
    }
    
    // Receive list size
    int list_size;
    if (recv_int(sockfd, &list_size) < 0) {
        printf("Error: Failed to receive file list size...\n");
        close(sockfd);
        return -1;
    }
    
    if (list_size <= 0) {
        if (list_size < 0) {
            printf("Error: Server could not build the file list\n");
            close(sockfd);
            return -1;
        }
        printf("No files found in directory: %s\n", pathname);
        close(sockfd);
        return 0;
    }
    
    // Receive and display file list
    char *file_list = malloc((size_t)list_size + 1);
    if (!file_list) {
        printf("Error: Memory allocation failed\n");
        close(sockfd);
        return -1;
    }
    
    if (recv_bytes(sockfd, file_list, list_size) != list_size) {
        printf("Error: Failed to receive file list\n");
        free(file_list);
        close(sockfd);
        return -1;
    }
    
    file_list[list_size] = '\0';
    close(sockfd);
    
    printf("\nFiles in directory %s:\n", pathname);
    printf("%s", file_list);
    
    free(file_list);
    return 0;
}

// Parse command line input into array of arguments
static int tokenize_command(char *input, char **args) {
    int argc = 0;
    char *token = strtok(input, " \t\n");
    
    while (token != NULL && argc < MAX_ARGS) {
        args[argc] = token;
        token = strtok(NULL, " \t\n");
        argc++;
    }
    if (token != NULL) {
        return MAX_ARGS + 1;
    }
    return argc;
}

static void print_help(void) {
    printf("Commands:\n"
           "  upload <local-file> [local-file ...] <remote-directory>\n"
           "  download <remote-file> [remote-file ...]\n"
           "  remove <remote-file> [remote-file ...]\n"
           "  archive <.c|.pdf|.txt|.zip>\n"
           "  list <remote-directory>\n"
           "  help\n"
           "  exit\n"
           "Remote paths start with /, for example /documents/report.pdf.\n"
           "Batches support up to %d files. Whitespace in paths is not supported.\n",
           FILEMESH_MAX_BATCH_FILES);
}

int main(void) {
    char input[BUFFER_SIZE];
    char *args[MAX_ARGS];
    int argc;

    signal(SIGPIPE, SIG_IGN);
    printf("FileMesh client. Type 'help' for commands.\n");
    
    while (1) {
        printf("filemesh> ");
        fflush(stdout);
        
        // Read input
        if (fgets(input, sizeof(input), stdin) == NULL) {
            putchar('\n');
            break;
        }

        // Parse command
        argc = tokenize_command(input, args);
        if (argc == 0) {
            continue;
        }
        if (argc > MAX_ARGS) {
            printf("Error: Too many command arguments...\n");
            continue;
        }
        
        // Handle commands
        if (strcmp(args[0], "exit") == 0) {
            break;
        } else if (strcmp(args[0], "help") == 0) {
            print_help();
        } else if (strcmp(args[0], "upload") == 0) {
            handle_upload_command(args, argc);
        } else if (strcmp(args[0], "download") == 0) {
            handle_download_command(args, argc);
        } else if (strcmp(args[0], "remove") == 0) {
            handle_remove_command(args, argc);
        } else if (strcmp(args[0], "archive") == 0) {
            handle_archive_command(args, argc);
        } else if (strcmp(args[0], "list") == 0) {
            handle_list_command(args, argc);
        } else {
            printf("Error: Unknown command. Type 'help' for commands.\n");
        }
    }
    return 0;
}
