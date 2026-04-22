#include "../include/Server.hpp"
#include "../include/Response.hpp"
#include "../include/Cgi.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <csignal>
#include <netinet/tcp.h>

Server::Server(std::vector<ServerConfig> &configs) : _configs(configs), _kq(-1) {
    _kq = epoll_create1(0);
    if (_kq == -1) throw std::runtime_error("epoll_create1() failed");
    setupListenSockets();
}

Server::~Server() {
    for (std::map<int, Client>::iterator it = _clients.begin(); it != _clients.end(); it++) {
        if (it->second.cgi_read_fd != -1) close(it->second.cgi_read_fd);
        if (it->second.cgi_write_fd != -1) close(it->second.cgi_write_fd);
        close(it->first);
    }
    for (std::map<int, int>::iterator it = _listen_fds.begin(); it != _listen_fds.end(); it++)
        close(it->first);
    if (_kq != -1) close(_kq);
}

void Server::setupListenSockets() {
    for (size_t i = 0; i < _configs.size(); i++) {
        bool already_bound = false;
        for (std::map<int, int>::iterator it = _listen_fds.begin(); it != _listen_fds.end(); it++) {
            ServerConfig &existing = _configs[it->second];
            if (existing.host == _configs[i].host && existing.port == _configs[i].port) {
                already_bound = true;
                break;
            }
        }
        if (already_bound) continue;
        int fd = createSocket(_configs[i]);
        _listen_fds[fd] = static_cast<int>(i);
        addEvent(fd, EPOLLIN, EPOLL_CTL_ADD);
        std::cout << "Listening on " << _configs[i].host << ":" << _configs[i].port << std::endl;
    }
}

int Server::createSocket(const ServerConfig &config) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) throw std::runtime_error("socket() failed");
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    fcntl(fd, F_SETFL, O_NONBLOCK);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    if (config.host == "0.0.0.0" || config.host.empty())
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(fd);
        std::ostringstream ss;
        ss << "bind failed on " << config.host << ":" << config.port;
        throw std::runtime_error(ss.str());
    }
    listen(fd, SOMAXCONN);
    return fd;
}

void Server::addEvent(int fd, int filter, int flags) {
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = filter;
    ev.data.fd = fd;
    epoll_ctl(_kq, flags, fd, &ev);
}

void Server::deleteEvent(int fd, int filter) {
    (void)filter;
    epoll_ctl(_kq, EPOLL_CTL_DEL, fd, NULL);
}

void Server::modifyEvent(int fd, int filter) {
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = filter;
    ev.data.fd = fd;
    epoll_ctl(_kq, EPOLL_CTL_MOD, fd, &ev);
}

void Server::run() {
    struct epoll_event events[MAX_EVENTS];
    while (true) {
        int n = epoll_wait(_kq, events, MAX_EVENTS, 5000);
        if (n == -1 && errno != EINTR) throw std::runtime_error("epoll_wait failed");
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (_cgi_pipe_fds.count(fd))       handleCgiRead(fd);
                else if (_cgi_write_fds.count(fd)) handleCgiWriteError(fd);
                else if (_clients.count(fd))       removeClient(fd);
                continue;
            }
            if (_listen_fds.count(fd))             handleNewConnection(fd);
            else if (_cgi_pipe_fds.count(fd))      handleCgiRead(fd);
            else if (_cgi_write_fds.count(fd))     handleCgiWrite(fd);
            else if (events[i].events & EPOLLIN)   handleRead(fd);
            else if (events[i].events & EPOLLOUT)  handleWrite(fd);
        }
        checkTimeouts();
    }
}

void Server::handleNewConnection(int listen_fd) {
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd == -1) return;
    fcntl(client_fd, F_SETFL, O_NONBLOCK);
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    Client client;
    client.fd            = client_fd;
    client.server        = matchServer(listen_fd);
    client.last_activity = time(NULL);
    client.keep_alive    = true;
    _clients[client_fd]  = client;
    addEvent(client_fd, EPOLLIN, EPOLL_CTL_ADD);
}

// handleRead: altijd lezen en bufferen in recv_buffer.
// Verwerken alleen als send_buffer leeg is (geen pipelining conflict).
void Server::handleRead(int fd) {
    if (!_clients.count(fd)) return;
    Client &client = _clients[fd];

    char buf[4096];
    ssize_t bytes = recv(fd, buf, sizeof(buf), 0);
    if (bytes <= 0) {
        removeClient(fd);
        return;
    }
    client.last_activity = time(NULL);

    // Streaming CGI: stuur body bytes direct naar CGI pipe, niet naar recv_buffer
    if (client.cgi_streaming && client.cgi_write_fd != -1) {
        client.cgi_body.append(buf, bytes);
        client.cgi_bytes_streamed += bytes;
        struct epoll_event _ev;
        std::memset(&_ev, 0, sizeof(_ev));
        _ev.events = EPOLLOUT;
        _ev.data.fd = client.cgi_write_fd;
        if (epoll_ctl(_kq, EPOLL_CTL_MOD, client.cgi_write_fd, &_ev) == -1)
            epoll_ctl(_kq, EPOLL_CTL_ADD, client.cgi_write_fd, &_ev);
        if (client.cgi_bytes_streamed >= client.cgi_body_total) {
            client.cgi_streaming = false;  // body compleet
        }
        return;
    }

    client.recv_buffer.append(buf, bytes);

    // Nog bezig met schrijven van vorige response: wacht, data staat in recv_buffer
    if (!client.send_buffer.empty())
        return;

    processBufferedRequests(client);
}

// processBufferedRequests: geef recv_buffer aan parser.
// Parser bewaart zijn eigen interne buffer, dus aanroepen met ""
// verwerkt ook data die al in de parser zat (pipelined requests).
void Server::processBufferedRequests(Client &client) {
    if (client.recv_buffer.empty()) return;
    if (!client.server) return;

    client.request.max_body_size = client.server->max_body_size;

    // Parse recv_buffer (mag leeg zijn: parser verwerkt dan zijn interne buffer)
    client.parser.parse(client.request, client.recv_buffer);
    client.recv_buffer.clear();

    if (client.request.hasError()) {
        client.parser.reset();
        client.send_buffer = buildErrorResponse(client, client.request.error_msg);
        client.keep_alive  = false;
        modifyEvent(client.fd, EPOLLOUT);
        return;
    }

    // Early CGI start: headers compleet maar body nog niet volledig ontvangen
    if (!client.cgi_running
        && client.request.state == PARSE_BODY
        && client.request.content_length > 0) {
        const Location *loc = client.server
            ? client.server->matchLocation(client.request.uri) : NULL;
        if (loc) {
            std::string filepath = client.request.uri.substr(loc->path.size());
            if (filepath.empty() || filepath[0] != '/') filepath = "/" + filepath;
            filepath = loc->root + filepath;
            const CgiConfig *cgi_cfg = NULL;
            size_t dot = filepath.rfind('.');
            if (dot != std::string::npos) {
                std::string ext = filepath.substr(dot);
                for (size_t i = 0; i < loc->cgi.size(); i++) {
                    if (loc->cgi[i].extension == ext) {
                        cgi_cfg = &loc->cgi[i];
                        break;
                    }
                }
            }
            if (cgi_cfg) {
                try {
                    CgiHandler cgi(client.request, *loc, filepath, *cgi_cfg);
                    pid_t pid; int write_fd;
                    int read_fd = cgi.start(pid, write_fd);
                    client.cgi_running  = true;
                    client.cgi_pid      = pid;
                    client.cgi_start    = time(NULL);
                    client.cgi_read_fd  = read_fd;
                    client.cgi_streaming       = true;
                    client.cgi_bytes_streamed  = 0;
                    _cgi_pipe_fds[read_fd] = client.fd;
                    addEvent(read_fd, EPOLLIN, EPOLL_CTL_ADD);
                    if (write_fd != -1) {
                        client.cgi_write_fd = write_fd;
                        // Dump wat de parser al heeft in cgi_body
                        client.cgi_body.swap(client.request.body);
                        client.cgi_bytes_streamed = client.cgi_body.size();
                        _cgi_write_fds[write_fd] = client.fd;
                        addEvent(write_fd, EPOLLOUT, EPOLL_CTL_ADD);
                    }
                    client.cgi_body_total = client.request.content_length;
                    std::string leftover = client.parser.getBuffer();
                    client.request.reset();
                    client.parser.reset();
                    client.recv_buffer.clear();
                    // Eerste body bytes zaten in parser buffer, niet in request.body
                    if (!leftover.empty()) {
                        client.cgi_body.append(leftover);
                        client.cgi_bytes_streamed += leftover.size();
                        if (client.cgi_bytes_streamed >= client.cgi_body_total)
                            client.cgi_streaming = false;
                    }
                } catch (...) {
                    client.send_buffer = buildErrorResponse(client, "500 CGI start failed");
                    client.keep_alive  = false;
                    modifyEvent(client.fd, EPOLLOUT);
                }
                return;
            }
        }
    }

    if (client.request.isComplete()) {
        processRequest(client);
    }
}

void Server::processRequest(Client &client) {
    bool is_head = (client.request.method == "HEAD");
    client.keep_alive = client.request.keepAlive();

    // Host-based server routing
    std::string host_header = client.request.getHeader("host");
    size_t colon = host_header.find(':');
    if (colon != std::string::npos)
        host_header = host_header.substr(0, colon);
    if (!host_header.empty() && client.server) {
        ServerConfig *matched = matchServerByHost(host_header, client.server->port);
        if (matched)
            client.server = matched;
    }

    const Location *loc = client.server
        ? client.server->matchLocation(client.request.uri)
        : NULL;

    ResponseBuilder builder;
    Response res = builder.build(client.request, *client.server, loc);

    // CGI
    if (res.isCgiPending()) {
        std::string filepath = res.getCgiFilepath();
        const CgiConfig *cgi_cfg = NULL;
        for (size_t i = 0; loc && i < loc->cgi.size(); i++) {
            size_t dot = filepath.rfind('.');
            if (dot != std::string::npos &&
                filepath.substr(dot) == loc->cgi[i].extension) {
                cgi_cfg = &loc->cgi[i];
                break;
            }
        }
        if (cgi_cfg) {
            try {
                CgiHandler handler(client.request, *loc, filepath, *cgi_cfg);
                pid_t pid;
                int write_fd;
                int pipe_fd = handler.start(pid, write_fd);
                client.cgi_pid         = pid;
                client.cgi_read_fd     = pipe_fd;
                client.cgi_running     = true;
                client.cgi_start       = time(NULL);
                client.cgi_output.clear();
                _cgi_pipe_fds[pipe_fd] = client.fd;
                addEvent(pipe_fd, EPOLLIN, EPOLL_CTL_ADD);
                if (write_fd != -1) {
                    client.cgi_write_fd      = write_fd;
                    client.cgi_body_offset   = 0;
                    client.cgi_body.swap(client.request.body);   // O(1) pointer swap ipv memcpy
                    _cgi_write_fds[write_fd] = client.fd;
                    addEvent(write_fd, EPOLLOUT, EPOLL_CTL_ADD);
                }
                client.recv_buffer = client.parser.getBuffer();
                client.request.reset();
                client.parser.reset();
            } catch (...) {
                client.send_buffer = buildErrorResponse(client, "500 CGI start failed");
                client.keep_alive  = false;
                client.recv_buffer.clear();
                client.request.reset();
                client.parser.reset();
                modifyEvent(client.fd, EPOLLOUT);
            }
        } else {
            client.send_buffer = buildErrorResponse(client, "500 No CGI config");
            client.keep_alive  = false;
            client.recv_buffer.clear();
            client.request.reset();
            client.parser.reset();
            modifyEvent(client.fd, EPOLLOUT);
        }
        return;
    }

    res.setHeader("Connection", client.keep_alive ? "keep-alive" : "close");
    std::string serialized = res.serialize(is_head ? "HEAD" : client.request.method);
    client.send_buffer.swap(serialized);

    client.recv_buffer = client.parser.getBuffer();

    client.request.reset();
    client.parser.reset();
    modifyEvent(client.fd, EPOLLOUT);
}

// handleWrite: schrijf send_buffer naar client.
// Na leegschrijven: verwerk pipelined data uit parser._buffer of recv_buffer.
void Server::handleWrite(int fd) {
    if (!_clients.count(fd)) return;
    Client &client = _clients[fd];

    if (client.send_buffer.empty()) {
        if (client.keep_alive) {
            if (!client.recv_buffer.empty()) {
                processBufferedRequests(client);
            } else {
                modifyEvent(fd, EPOLLIN);
            }
        } else {
            removeClient(fd);
        }
        return;
    }

    ssize_t bytes = send(fd, client.send_buffer.c_str(), client.send_buffer.size(), 0);
    if (bytes <= 0) {
        removeClient(fd);
        return;
    }
    client.send_buffer.erase(0, bytes);

    if (client.send_buffer.empty()) {
        if (client.keep_alive) {
            processBufferedRequests(client);
            if (client.send_buffer.empty() && !client.cgi_running) {
                modifyEvent(fd, EPOLLIN);
            }
        } else {
            removeClient(fd);
        }
    }
}

void Server::handleCgiRead(int pipe_fd) {
    if (!_cgi_pipe_fds.count(pipe_fd)) return;
    int client_fd = _cgi_pipe_fds[pipe_fd];
    if (!_clients.count(client_fd)) return;
    Client &client = _clients[client_fd];

    char buf[4096];
    ssize_t bytes = read(pipe_fd, buf, sizeof(buf));
    if (bytes > 0) {
        client.cgi_output.append(buf, bytes);
        client.last_activity = time(NULL);
    } else {
        epoll_ctl(_kq, EPOLL_CTL_DEL, pipe_fd, NULL);
        close(pipe_fd);
        _cgi_pipe_fds.erase(pipe_fd);
        client.cgi_read_fd = -1;

        waitpid(client.cgi_pid, NULL, 0);
        client.cgi_pid     = -1;
        client.cgi_running = false;

        Response res = CgiHandler::parseCgiOutput(client.cgi_output, *client.server);
        std::string().swap(client.cgi_output);
        res.setHeader("Connection", client.keep_alive ? "keep-alive" : "close");
        std::string serialized = res.serialize("GET");
        client.send_buffer.swap(serialized);
        modifyEvent(client_fd, EPOLLOUT);
    }
}

void Server::handleCgiWrite(int write_fd) {
    if (!_cgi_write_fds.count(write_fd)) return;
    int client_fd = _cgi_write_fds[write_fd];
    if (!_clients.count(client_fd)) return;
    Client &client = _clients[client_fd];

    if (client.cgi_body.empty()) {
        if (!client.cgi_streaming) {
            epoll_ctl(_kq, EPOLL_CTL_DEL, write_fd, NULL);
            close(write_fd);
            _cgi_write_fds.erase(write_fd);
            client.cgi_write_fd = -1;
        } else {
            // Nog data onderweg: tijdelijk uit epoll halen om busy-loop te vermijden.
            // handleRead voegt hem terug toe zodra er data is.
            epoll_ctl(_kq, EPOLL_CTL_DEL, write_fd, NULL);
        }
        return;
    }

    ssize_t bytes = write(write_fd, client.cgi_body.c_str(), client.cgi_body.size());
    if (bytes > 0) {
        client.cgi_body.erase(0, bytes);
        if (client.cgi_body.empty() && !client.cgi_streaming) {
            epoll_ctl(_kq, EPOLL_CTL_DEL, write_fd, NULL);
            close(write_fd);
            _cgi_write_fds.erase(write_fd);
            client.cgi_write_fd = -1;
        }
    } else {
        epoll_ctl(_kq, EPOLL_CTL_DEL, write_fd, NULL);
        close(write_fd);
        _cgi_write_fds.erase(write_fd);
        client.cgi_write_fd = -1;
        client.cgi_body.clear();
    }
}

void Server::handleCgiWriteError(int write_fd) {
    if (!_cgi_write_fds.count(write_fd)) return;
    epoll_ctl(_kq, EPOLL_CTL_DEL, write_fd, NULL);
    close(write_fd);
    _cgi_write_fds.erase(write_fd);
}

void Server::removeClient(int fd) {
    if (!_clients.count(fd)) return;
    Client &client = _clients[fd];
    if (client.cgi_running) {
        kill(client.cgi_pid, SIGKILL);
        waitpid(client.cgi_pid, NULL, 0);
    }
    if (client.cgi_read_fd != -1) {
        epoll_ctl(_kq, EPOLL_CTL_DEL, client.cgi_read_fd, NULL);
        close(client.cgi_read_fd);
        _cgi_pipe_fds.erase(client.cgi_read_fd);
    }
    if (client.cgi_write_fd != -1) {
        epoll_ctl(_kq, EPOLL_CTL_DEL, client.cgi_write_fd, NULL);
        close(client.cgi_write_fd);
        _cgi_write_fds.erase(client.cgi_write_fd);
    }
    epoll_ctl(_kq, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    _clients.erase(fd);
}

void Server::checkTimeouts() {
    time_t now = time(NULL);
    std::vector<int> to_remove;
    for (std::map<int, Client>::iterator it = _clients.begin();
         it != _clients.end(); it++) {
        if (it->second.cgi_running && now - it->second.cgi_start > CGI_TIMEOUT) {
            kill(it->second.cgi_pid, SIGKILL);
            waitpid(it->second.cgi_pid, NULL, WNOHANG);
            it->second.cgi_running = false;
            it->second.send_buffer = buildErrorResponse(it->second, "504 Gateway Timeout");
            modifyEvent(it->first, EPOLLOUT);
        } else if (now - it->second.last_activity > TIMEOUT_SEC) {
            to_remove.push_back(it->first);
        }
    }
    for (size_t i = 0; i < to_remove.size(); i++)
        removeClient(to_remove[i]);
}

ServerConfig *Server::matchServer(int listen_fd) {
    if (_listen_fds.count(listen_fd))
        return &_configs[_listen_fds[listen_fd]];
    return NULL;
}

ServerConfig *Server::matchServerByHost(const std::string &host, int port) {
    ServerConfig *fallback = NULL;
    for (size_t i = 0; i < _configs.size(); i++) {
        if (_configs[i].port == port) {
            if (!fallback) fallback = &_configs[i];
            if (_configs[i].server_name == host) return &_configs[i];
        }
    }
    return fallback;
}

std::string Server::buildErrorResponse(Client &client, const std::string &error_msg) {
    int code = 500;
    if (error_msg.size() >= 3)
        code = std::atoi(error_msg.substr(0, 3).c_str());
    ResponseBuilder builder;
    Response res = builder.serveErrorPage(
        code, client.server ? *client.server : _configs[0]);
    res.setHeader("Connection", client.keep_alive ? "keep-alive" : "close");
    return res.serialize("GET");
}