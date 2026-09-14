#define FILEMESH_ENABLE_STORAGE_SERVER
#include "server_utils.h"

int main(void) {
    const StorageServerConfig config = {
        .server_name = "ZIP",
        .directory_name = "zip",
        .extension = ".zip",
        .type_label = "ZIP",
        .port = FILEMESH_ZIP_PORT,
        .backlog = 16
    };
    return run_storage_server(&config);
}
