#ifndef SERVER_HPP
#define SERVER_HPP

#include "Config.hpp"
#include "Request.hpp"
#include <vector>
#include <map>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __APPLE__
# include <sys/event.h>   // kqueue - macOS
#else
# include <sys/epoll.h>   // epoll - Linux
#endif

#define MAX_EVENTS  64
#define TIMEOUT_SEC 60  // client timeout in seconden
#define CGI_TIMEOUT 5   // CGI timeout in seconden

// Ruwe request/response buffers per client
struct Client {
    int                 fd;
    std::string         recv_buffer;
    std::string         send_buffer;
    ServerConfig        *server;
    time_t              last_activity;
    bool                keep_alive;
    Request             request;
    RequestParser       parser;

    // CGI state
    pid_t               cgi_pid;
    int                 cgi_read_fd;    // read uiteinde van de CGI output pipe
    int                 cgi_write_fd;   // write uiteinde voor CGI stdin (async body write)
    size_t              cgi_body_offset; // hoeveel bytes al geschreven naar CGI stdin
    std::string         cgi_output;     // accumuleert CGI stdout
    bool                cgi_running;
    time_t              cgi_start;      // voor timeout detectie

    Client()
        : fd(-1)
        , server(NULL)
        , last_activity(0)
        , keep_alive(true)
        , cgi_pid(-1)
        , cgi_read_fd(-1)
        , cgi_write_fd(-1)
        , cgi_body_offset(0)
        , cgi_running(false)
        , cgi_start(0)
    {}
};

// De server class
class Server {
public:
    Server(std::vector<ServerConfig> &configs);
    ~Server();

    void    run();

private:
    std::vector<ServerConfig>   &_configs;
    int                         _kq;
    std::map<int, int>          _listen_fds;    // listen fd -> index in _configs
    std::map<int, Client>       _clients;       // client fd -> client
    std::map<int, int>          _cgi_pipe_fds;  // cgi read pipe fd -> client fd
    std::map<int, int>          _cgi_write_fds; // cgi write pipe fd -> client fd

    // Setup
    void    setupListenSockets();
    int     createSocket(const ServerConfig &config);
    void    addEvent(int fd, int filter, int flags);
    void    deleteEvent(int fd, int filter);
    void    modifyEvent(int fd, int filter);

    // Event handlers
    void    handleNewConnection(int listen_fd);
    void    handleRead(int fd);
    void    handleWrite(int fd);
    void    handleCgiRead(int pipe_fd);
    void    handleCgiWrite(int write_fd);
    void    handleCgiWriteError(int write_fd);
    void    removeClient(int fd);
    void    checkTimeouts();

    // Request verwerking
    void        processRequest(Client &client);
    void processBufferedRequests(Client &client);
    std::string buildErrorResponse(Client &client, const std::string &error_msg);

    // Hulpfuncties
    ServerConfig    *matchServer(int listen_fd);
    ServerConfig    *matchServerByHost(const std::string &host, int port);
};

#endif