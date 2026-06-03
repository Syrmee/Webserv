*This project has been created as part of the 42 curriculum by <login>.*

# Webserv

## Description

Webserv is a small HTTP web server implemented in C++98. It is designed to follow the 42 curriculum objectives by implementing a minimal, event-driven web server that can parse configuration files, handle HTTP requests, serve static files, provide directory autoindexing, manage file uploads, and execute CGI scripts.

The project uses non-blocking sockets and `poll()` to manage multiple connections in a single-threaded event loop. It also implements HTTP request parsing, response building, and simple CGI support for dynamic content execution.

## Instructions

### Compilation

1. Open a terminal in the project root.
2. Run:

```sh
make
```

This builds the executable named `webserv`.

### Execution

Run the server with a configuration file. For example:

```sh
./webserv test.conf
```

or:

```sh
./webserv eval.conf
```

### Clean build artifacts

```sh
make fclean
```

## Usage

- Use a `.conf` file to configure server blocks, listen ports, root directories, CGI handlers, error pages, and upload locations.
- The server supports methods such as `GET` and `POST`.
- It can serve static files, generate autoindex pages for directories, and execute CGI scripts.

## Features

- C++98 compliant implementation
- Non-blocking I/O with `poll()`
- HTTP request parsing and validation
- Static file serving
- Directory autoindex generation
- File upload handling
- CGI execution with environment variables and request body forwarding
- Configuration file parsing for multiple server and location blocks

## Resources

- HTTP/1.1 specification: RFC 7230 / RFC 7231
- CGI protocol: RFC 3875
- `poll()` system call documentation
- 42 Network and Webserv subject resources

## AI Usage

This README was created with help from an AI assistant to summarize the project, organize the documentation, and ensure the required sections were present. The AI was used only for drafting the documentation content, not for implementing the server logic.