#define FILEMESH_ENABLE_STORAGE_SERVER
#include "server_utils.h"

int main(void) {
    const StorageServerConfig config = {
        .server_name = "Text",
        .directory_name = "text",
        .extension = ".txt",
        .type_label = "Text",
        .port = FILEMESH_TEXT_PORT,
        .backlog = 16
    };
    return run_storage_server(&config);
}
