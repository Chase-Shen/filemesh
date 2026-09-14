#define FILEMESH_ENABLE_STORAGE_SERVER
#include "server_utils.h"

int main(void) {
    const StorageServerConfig config = {
        .server_name = "PDF",
        .directory_name = "pdf",
        .extension = ".pdf",
        .type_label = "PDF",
        .port = FILEMESH_PDF_PORT,
        .backlog = 16
    };
    return run_storage_server(&config);
}
