#ifndef CGI_HPP
#define CGI_HPP

#include "Request.hpp"
#include "Config.hpp"
#include "Response.hpp"
#include <sys/types.h>
#include <string>
#include <map>

// CGI handler
// start() forkt het CGI script en geeft de read pipe fd terug.
// De Server leest de output via kqueue/epoll.
// Bij POST requests met body wordt ook de write pipe fd teruggegeven
// zodat de Server de body asynchroon kan schrijven.
class CgiHandler {
public:
    CgiHandler(const Request &req,
               const Location &loc,
               const std::string &filepath,
               const CgiConfig &cgi);
    ~CgiHandler();

    // Forkt het CGI script.
    // Geeft de read fd van de output pipe terug.
    // Slaat de pid op in out_pid.
    // Slaat de write fd op in out_write_fd (-1 als geen body).
    // Gooit std::runtime_error bij fout.
    int start(pid_t &out_pid, int &out_write_fd);

    // Parset de ruwe CGI output string naar een Response object.
    // Statische functie: kan ook door Server aangeroepen worden.
    static Response parseCgiOutput(const std::string &raw_output,
                                   const ServerConfig &server);

private:
    const Request      &_req;
    const Location     &_loc;
    const std::string  _filepath;
    const CgiConfig    _cgi;

    std::vector<std::string>    buildEnv() const;
    char                        **envToCharpp(const std::vector<std::string> &env) const;
    void                        freeCharpp(char **arr) const;
    std::string                 getScriptDir() const;
    std::string                 getScriptName() const;
};

#endif