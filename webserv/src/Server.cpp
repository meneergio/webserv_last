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
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <csignal>

// Constructor / Destructor

Server::Server(std::vector<ServerConfig> &configs)
    : _configs(configs)
    , _kq(-1)
{
    _kq = kqueue();
    if (_kq == -1)
        throw std::runtime_error("kqueue() failed");

    setupListenSockets();
}

Server::~Server() {
    for (std::map<int, Client>::iterator it = _clients.begin();
         it != _clients.end(); it++) {
        if (it->second.cgi_read_fd != -1)
            close(it->second.cgi_read_fd);
        close(it->first);
    }
    for (std::map<int, int>::iterator it = _listen_fds.begin();
         it != _listen_fds.end(); it++) {
        close(it->first);
    }
    if (_kq != -1)
        close(_kq);
}

// Setup

void Server::setupListenSockets() {
    for (size_t i = 0; i < _configs.size(); i++) {
        bool already_bound = false;
        for (std::map<int, int>::iterator it = _listen_fds.begin();
             it != _listen_fds.end(); it++) {
            ServerConfig &existing = _configs[it->second];
            if (existing.host == _configs[i].host
                && existing.port == _configs[i].port) {
                already_bound = true;
                break;
            }
        }
        if (already_bound) {
            std::cout << "Sharing socket for "
                      << _configs[i].host << ":"
                      << _configs[i].port
                      << " (server_name: " << _configs[i].server_name << ")"
                      << std::endl;
            continue;
        }

        int fd = createSocket(_configs[i]);
        _listen_fds[fd] = static_cast<int>(i);
        addEvent(fd, EVFILT_READ, EV_ADD);
        std::cout << "Listening on "
                  << _configs[i].host << ":"
                  << _configs[i].port << std::endl;
    }
}

int Server::createSocket(const ServerConfig &config) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        throw std::runtime_error("socket() failed");

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        close(fd);
        throw std::runtime_error("setsockopt() failed");
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        close(fd);
        throw std::runtime_error("fcntl() failed");
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);

    if (config.host == "0.0.0.0" || config.host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) != 1) {
            close(fd);
            throw std::runtime_error("Invalid host address: " + config.host);
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(fd);
        throw std::runtime_error("bind() failed on port " + config.host);
    }

    if (listen(fd, SOMAXCONN) == -1) {
        close(fd);
        throw std::runtime_error("listen() failed");
    }

    return fd;
}

// kqueue helpers

void Server::addEvent(int fd, int filter, int flags) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, flags, 0, 0, NULL);
    if (kevent(_kq, &ev, 1, NULL, 0, NULL) == -1)
        throw std::runtime_error("kevent() add failed");
}

void Server::deleteEvent(int fd, int filter) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, NULL);
    kevent(_kq, &ev, 1, NULL, 0, NULL);
}

void Server::modifyEvent(int fd, int filter) {
    (void)fd; (void)filter;
}

// Hoofd event loop

void Server::run() {
    struct kevent events[MAX_EVENTS];
    std::cout << "Server running..." << std::endl;

    while (true) {
        struct timespec timeout;
        timeout.tv_sec  = 5;
        timeout.tv_nsec = 0;

        int n = kevent(_kq, NULL, 0, events, MAX_EVENTS, &timeout);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("kevent() wait failed");
        }

        for (int i = 0; i < n; i++) {
            int fd = static_cast<int>(events[i].ident);

            if (events[i].flags & EV_ERROR) {
                if (_cgi_pipe_fds.count(fd))
                    handleCgiRead(fd);
                else if (_clients.count(fd))
                    removeClient(fd);
                continue;
            }

            if (_listen_fds.count(fd)) {
                handleNewConnection(fd);
            } else if (_cgi_pipe_fds.count(fd)) {
                handleCgiRead(fd);
            } else if (events[i].filter == EVFILT_READ) {
                handleRead(fd);
            } else if (events[i].filter == EVFILT_WRITE) {
                handleWrite(fd);
            }
        }

        checkTimeouts();
    }
}

// Nieuwe verbinding

void Server::handleNewConnection(int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd == -1)
        return;

    if (fcntl(client_fd, F_SETFL, O_NONBLOCK) == -1) {
        close(client_fd);
        return;
    }

    Client client;
    client.fd            = client_fd;
    client.server        = matchServer(listen_fd);
    client.last_activity = time(NULL);
    client.keep_alive    = false;
    _clients[client_fd]  = client;

    addEvent(client_fd, EVFILT_READ, EV_ADD);
    std::cout << "New connection: fd=" << client_fd << std::endl;
}

// Data lezen van client

void Server::handleRead(int fd) {
    if (!_clients.count(fd)) return;

    Client &client = _clients[fd];
    char buf[8192];

    ssize_t bytes = recv(fd, buf, sizeof(buf), 0);
    if (bytes <= 0) { removeClient(fd); return; }

    client.last_activity = time(NULL);
    if (client.server)
        client.request.max_body_size = client.server->max_body_size;

    // 1. Maak een string van de nieuwe data
    std::string data(buf, bytes);
    
    // 2. Geef de data door bij de EERSTE aanroep van parse
    bool first_call = true;

    while (true) {
        // Geef 'data' alleen de eerste keer mee, daarna lege strings 
        // zodat de parser de overgebleven data in zijn interne buffer verwerkt.
        client.parser.parse(client.request, first_call ? data : "");
        first_call = false;

        if (client.request.hasError()) {
            client.send_buffer += buildErrorResponse(client, client.request.error_msg);
            client.keep_alive = false;
            addEvent(fd, EVFILT_WRITE, EV_ADD);
            break;
        }

        if (client.request.isComplete()) {
            processRequest(client);
            // Als de verbinding gesloten moet worden (bijv. na error), stop de loop
            if (!client.keep_alive || _clients.count(fd) == 0) break;
            
            // Belangrijk: De parser moet intern gereset zijn in processRequest 
            // maar de resterende data in zijn buffer hebben behouden.
            continue; 
        }
        break; 
    }
}

// ----------------------------------------
// processRequest: bouw response of start CGI
// ----------------------------------------

void Server::processRequest(Client &client) {
    const Request &req = client.request;
    int fd = client.fd;


    // 1. Connection bepalen (Belangrijk voor tester stabiliteit)
    client.keep_alive = req.keepAlive();

    // 2. Host-based routing
    std::string host_header = req.getHeader("host");
    size_t colon = host_header.find(':');
    if (colon != std::string::npos)
        host_header = host_header.substr(0, colon);
    
    if (!host_header.empty() && client.server) {
        ServerConfig *matched = matchServerByHost(host_header, client.server->port);
        if (matched)
            client.server = matched;
    }

    // 3. Match Locatie
    const Location *loc = NULL;
    if (client.server)
        loc = client.server->matchLocation(req.uri);

    ResponseBuilder builder;
    Response res = builder.build(req, *client.server, loc);

    // 4. CGI Logica
    if (res.isCgiPending() && loc) {
        std::string filepath = res.getCgiFilepath();
        const CgiConfig *cgi_cfg = NULL;
        for (size_t i = 0; i < loc->cgi.size(); i++) {
            size_t dot = filepath.rfind('.');
            if (dot != std::string::npos && filepath.substr(dot) == loc->cgi[i].extension) {
                cgi_cfg = &loc->cgi[i];
                break;
            }
        }
        if (cgi_cfg) {
            try {
                CgiHandler handler(req, *loc, filepath, *cgi_cfg);
                pid_t pid;
                int pipe_fd = handler.start(pid);
                client.cgi_pid      = pid;
                client.cgi_read_fd  = pipe_fd;
                client.cgi_running  = true;
                client.cgi_start    = time(NULL);
                client.cgi_output.clear();
                _cgi_pipe_fds[pipe_fd] = fd;
                addEvent(pipe_fd, EVFILT_READ, EV_ADD);
                return; // CGI draait, we wachten op handleCgiRead
            } catch (const std::exception &e) {
                client.send_buffer += buildErrorResponse(client, "500 CGI start failed");
                client.keep_alive  = false;
            }
        } else {
            client.send_buffer += buildErrorResponse(client, "500 No CGI config");
            client.keep_alive  = false;
        }
    } else {
        // 5. Normale Response (Static / Error)
        // Zorg dat Connection header ALTIJD klopt voor de tester
        res.setHeader("Connection", client.keep_alive ? "keep-alive" : "close");
        
        // CRUCIAAL 1: Gebruik += voor pipelining (meerdere requests in 1 keer)
        // CRUCIAAL 2: Geef req.method mee zodat HEAD geen body stuurt!
        client.send_buffer += res.serialize(req.method);
    }

    // 6. Voorbereiden op het volgende request in de stream
    // Dit zorgt dat de \n na een POST niet als een nieuw request-begin wordt gezien
    client.request.reset(); 
    
    // Zorg dat je parser ook terug naar start-state gaat zonder de buffer te legen
    // (Voeg deze methode toe aan je RequestParser als die er nog niet is)
    // client.parser.resetStatusOnly(); 

    // 7. Registreer voor write-event
    addEvent(fd, EVFILT_WRITE, EV_ADD);
}

// ----------------------------------------
// handleCgiRead: lees CGI output via kqueue
// ----------------------------------------

void Server::handleCgiRead(int pipe_fd) {
    if (!_cgi_pipe_fds.count(pipe_fd))
        return;

    int client_fd = _cgi_pipe_fds[pipe_fd];
    if (!_clients.count(client_fd)) {
        deleteEvent(pipe_fd, EVFILT_READ);
        close(pipe_fd);
        _cgi_pipe_fds.erase(pipe_fd);
        return;
    }

    Client &client = _clients[client_fd];
    char buf[4096];
    ssize_t bytes = read(pipe_fd, buf, sizeof(buf));

    if (bytes > 0) {
        client.cgi_output.append(buf, bytes);
        client.last_activity = time(NULL);
        return;
    }

    deleteEvent(pipe_fd, EVFILT_READ);
    close(pipe_fd);
    _cgi_pipe_fds.erase(pipe_fd);
    client.cgi_read_fd = -1;

    waitpid(client.cgi_pid, NULL, 0);
    client.cgi_pid     = -1;
    client.cgi_running = false;

    Response res = CgiHandler::parseCgiOutput(
        client.cgi_output, *client.server);
    res.setHeader("Connection", client.keep_alive ? "keep-alive" : "close");
    client.send_buffer = res.serialize();
    client.request.reset();
    client.parser = RequestParser();

    addEvent(client_fd, EVFILT_WRITE, EV_ADD);
    deleteEvent(client_fd, EVFILT_READ);
}

// Response sturen

void Server::handleWrite(int fd) {
    if (!_clients.count(fd))
        return;

    Client &client = _clients[fd];

    if (client.send_buffer.empty()) {
        removeClient(fd);
        return;
    }

    ssize_t bytes = send(fd, client.send_buffer.c_str(),
                         client.send_buffer.size(), 0);

    if (bytes < 0) {
        removeClient(fd);
        return;
    }

    client.send_buffer.erase(0, bytes);
    client.last_activity = time(NULL);

    if (client.send_buffer.empty()) {
        if (client.keep_alive) {
            client.recv_buffer.clear();
            deleteEvent(fd, EVFILT_WRITE);
            addEvent(fd, EVFILT_READ, EV_ADD);
        } else {
            removeClient(fd);
        }
    }
}

// Client verwijderen

void Server::removeClient(int fd) {
    if (!_clients.count(fd))
        return;

    Client &client = _clients[fd];

    if (client.cgi_running) {
        kill(client.cgi_pid, SIGKILL);
        waitpid(client.cgi_pid, NULL, 0);
    }
    if (client.cgi_read_fd != -1) {
        deleteEvent(client.cgi_read_fd, EVFILT_READ);
        close(client.cgi_read_fd);
        _cgi_pipe_fds.erase(client.cgi_read_fd);
    }

    deleteEvent(fd, EVFILT_READ);
    deleteEvent(fd, EVFILT_WRITE);
    close(fd);
    _clients.erase(fd);

    std::cout << "Connection closed: fd=" << fd << std::endl;
}

// Timeouts: clients én CGI processen

void Server::checkTimeouts() {
    time_t now = time(NULL);
    std::vector<int> to_remove;

    for (std::map<int, Client>::iterator it = _clients.begin();
         it != _clients.end(); it++) {
        Client &client = it->second;

        if (client.cgi_running && now - client.cgi_start > CGI_TIMEOUT) {
            std::cout << "CGI timeout: fd=" << it->first << std::endl;

            kill(client.cgi_pid, SIGKILL);
            waitpid(client.cgi_pid, NULL, 0);
            client.cgi_pid     = -1;
            client.cgi_running = false;

            if (client.cgi_read_fd != -1) {
                deleteEvent(client.cgi_read_fd, EVFILT_READ);
                close(client.cgi_read_fd);
                _cgi_pipe_fds.erase(client.cgi_read_fd);
                client.cgi_read_fd = -1;
            }

            Response res;
            res.status_code = 504;
            res.status_msg  = "Gateway Timeout";
            res.setBody("<html><body><h1>504 Gateway Timeout</h1></body></html>",
                        "text/html");
            res.setHeader("Connection", "close");
            client.send_buffer = res.serialize();
            client.keep_alive  = false;
            client.request.reset();
            client.parser = RequestParser();

            try { addEvent(it->first, EVFILT_WRITE, EV_ADD); } catch (...) {}
            deleteEvent(it->first, EVFILT_READ);
            continue;
        }

        if (now - client.last_activity > TIMEOUT_SEC)
            to_remove.push_back(it->first);
    }

    for (size_t i = 0; i < to_remove.size(); i++) {
        std::cout << "Timeout: fd=" << to_remove[i] << std::endl;
        removeClient(to_remove[i]);
    }
}

// Hulpfuncties

ServerConfig *Server::matchServer(int listen_fd) {
    std::map<int, int>::iterator it = _listen_fds.find(listen_fd);
    if (it == _listen_fds.end())
        return NULL;
    return &_configs[it->second];
}

ServerConfig *Server::matchServerByHost(const std::string &host, int port) {
    ServerConfig *fallback = NULL;

    for (size_t i = 0; i < _configs.size(); i++) {
        if (_configs[i].port != port)
            continue;
        if (!fallback)
            fallback = &_configs[i];
        if (_configs[i].server_name == host)
            return &_configs[i];
    }
    return fallback;
}

std::string Server::buildErrorResponse(Client &client, const std::string &error_msg) {
    int code = 500;
    if (error_msg.size() >= 3)
        code = std::atoi(error_msg.substr(0, 3).c_str());

    ResponseBuilder builder;
    Response res = builder.build(
        client.request,
        client.server ? *client.server : _configs[0],
        NULL
    );
    res.status_code = code;
    res.status_msg  = error_msg.size() > 4 ? error_msg.substr(4) : "Error";
    res.setHeader("Connection", "close");
    return res.serialize();
}