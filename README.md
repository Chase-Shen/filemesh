# FileMesh — Distributed File Storage System

A small distributed file-storage prototype written in C. One gateway presents a
single remote directory tree while routing files to separate storage processes
according to their extension.

This project is an extension of a systems course project.

FileMesh explores TCP framing, streamed file transfers, process-per-connection
concurrency, and a shared implementation for storage nodes. It is intended for
local experimentation, not public hosting or production data.

## Features

- Upload, download, and remove files through an interactive client.
- Route `.c`, `.pdf`, `.txt`, and `.zip` files to their respective storage nodes.
- Use remote paths such as `/documents/report.pdf` without knowing where a file
  lives on disk.
- List directory contents, grouped by type and sorted alphabetically within each
  group.
- Download a recursive tar archive for any supported file type.
- Process up to 32 files in an upload, download, or removal batch.
- Share socket, transfer, path, archive, and storage-node logic in
  `include/server_utils.h`.

## Architecture

| Process | Role | Address |
| --- | --- | --- |
| Gateway | Client entry point; stores `.c` files and forwards other types | `127.0.0.1:11111` |
| PDF node | Stores `.pdf` files | `127.0.0.1:22222` |
| Text node | Stores `.txt` files | `127.0.0.1:33333` |
| ZIP node | Stores `.zip` files | `127.0.0.1:44444` |

Clients only connect to the gateway. Each server forks a child process for a
connection. Transfers use length-prefixed frames with 32-bit network-order
integers; payloads are streamed in 64 KiB chunks rather than loaded entirely into
memory.

## Build

Use Linux, a C11 compiler (GCC or Clang), GNU Make, and GNU tar. On Windows, use a
Linux environment such as WSL; the servers use POSIX sockets and `fork()` and do
not build as native Windows programs.

```sh
make
make check
```

Executables are placed in `build/`. To use Clang, run `make clean` followed by
`make CC=clang`.

## Run

Start each server in a separate terminal:

```sh
./build/pdf_server
./build/text_server
./build/zip_server
./build/gateway
```

Then start the client:

```sh
./build/client
```

Example client session, assuming the local files already exist:

```text
filemesh> upload ./examples/demo.c ./notes.txt /demo
filemesh> list /demo
filemesh> download /demo/notes.txt
filemesh> archive .txt
filemesh> remove /demo/demo.c /demo/notes.txt
filemesh> help
filemesh> exit
```

The client does not invoke a shell: `/demo` is a FileMesh directory, not your
computer's `/demo` directory. Local upload paths can be relative or absolute;
only the basename is stored remotely. Destination directories are created as
needed. Downloads are saved by basename in the client's working directory and
overwrite existing files with that name.

## Commands

| Command | Syntax |
| --- | --- |
| Upload | `upload <local-file> [local-file ...] <remote-directory>` |
| Download | `download <remote-file> [remote-file ...]` |
| Remove | `remove <remote-file> [remote-file ...]` |
| Archive | `archive <.c\|.pdf\|.txt\|.zip>` |
| List | `list <remote-directory>` |
| Help / quit | `help` / `exit` |

Remote paths must start with `/`; `/` itself is the root directory. Path traversal
components (`.` and `..`) and backslashes are rejected. Extensions are
case-sensitive. The interactive parser does not support quoted paths or
whitespace in filenames.

Archives are saved as `cfiles.tar`, `pdf.tar`, `text.tar`, or `zip.tar` in the
client's working directory. Archive entries use relative paths, preserve nested
folders, and exclude symbolic links.

## Storage configuration

By default, node data is stored beneath `$HOME/.local/share/filemesh/`, in
`source/`, `pdf/`, `text/`, and `zip/` subdirectories.

To choose another base directory, set the same absolute path in each server's
terminal before starting it:

```sh
export FILEMESH_DATA_DIR="$PWD/data"
```

Ports and the localhost address are centralized in `include/server_utils.h`.
The new storage layout does not move data from earlier versions automatically.
