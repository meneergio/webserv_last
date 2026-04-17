*This project has been created as part of the 42 curriculum by gwindey, dzotti.*

# Webserv

## Description

Webserv is an HTTP/1.1 server written in C++98, built from scratch without any external libraries. It handles multiple simultaneous clients using a non-blocking I/O event loop powered by `kqueue` (macOS). The server reads a configuration file inspired by NGINX syntax and supports static file serving, file uploads, directory listing, multiple ports, and CGI execution.

## Instructions

**Requirements**
- macOS
- `c++` with C++98 support
- `make`

**Compilation**
```bash
make
```

**Usage**
```bash
./webserv default.conf
```

Always run the server from the project root so that relative paths in the config file resolve correctly.

**Cleanup**
```bash
make clean   # removes object files
make fclean  # removes object files and binary
make re      # full recompile
```

## Configuration

The configuration file uses an NGINX-inspired syntax. Example:

```nginx
server {
    host        127.0.0.1;
    port        8080;
    server_name localhost;
    max_body_size 1M;
    error_page  404 /errors/404.html;

    location / {
        root            ./www;
        index           index.html;
        methods         GET POST;
        directory_listing off;
    }

    location /upload {
        root            ./www/uploads;
        methods         GET POST DELETE;
        upload_dir      ./www/uploads;
        max_body_size   10M;
    }

    location /cgi-bin {
        root            ./www/cgi-bin;
        methods         GET POST;
        cgi             .py /usr/bin/python3;
    }
}
```

**Supported directives:**
- `host`, `port`, `server_name`
- `max_body_size` (supports `K`, `M`, `G` suffixes)
- `error_page <code> <path>`
- `location` blocks with: `root`, `index`, `methods`, `directory_listing`, `upload_dir`, `redirect`, `cgi`, `max_body_size`

## Features

- Non-blocking I/O with `kqueue`
- Multiple servers and ports from a single config file
- GET, POST, DELETE methods
- Static file serving with correct MIME types
- Directory listing
- File uploads
- Custom error pages
- Client timeout handling
- Keep-alive connections
- Chunked transfer encoding
- CGI execution (PHP, Python)
- 413 body size enforcement

## Resources

**HTTP protocol**
- [RFC 7230 - HTTP/1.1 Message Syntax](https://datatracker.ietf.org/doc/html/rfc7230)
- [RFC 7231 - HTTP/1.1 Semantics](https://datatracker.ietf.org/doc/html/rfc7231)
- [MDN HTTP documentation](https://developer.mozilla.org/en-US/docs/Web/HTTP)

**Networking / sockets**
- [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/)
- `man kqueue`, `man kevent`, `man socket`, `man poll`

**NGINX reference**
- [NGINX configuration documentation](https://nginx.org/en/docs/)

**AI usage**

Claude (Anthropic) was used as a support tool during this project for the following:
- Discussing the overall architecture and how to split responsibilities between team members
- Clarifying concepts such as the kqueue event loop, HTTP parsing, and chunked transfer encoding
- Reviewing edge cases from the evaluation sheet (errno restrictions, single poll requirement, 413 handling)

All design decisions, implementation choices, and final code were made and written by the team. AI was used to discuss and verify ideas, not to generate code directly.