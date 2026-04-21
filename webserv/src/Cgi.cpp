#include "../include/Cgi.hpp"
#include "../include/Response.hpp" 
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cerrno>

CgiHandler::CgiHandler(const Request &req,
                       const Location &loc,
                       const std::string &filepath,
                       const CgiConfig &cgi)
    : _req(req)
    , _loc(loc)
    , _filepath(filepath)
    , _cgi(cgi)
{
    (void)_loc;
}

CgiHandler::~CgiHandler() {}

// start(): fork het script, geef read fd en write fd terug aan Server.
// Server schrijft body naar write fd en leest output van read fd via epoll.

int CgiHandler::start(pid_t &out_pid, int &out_write_fd) {
    int in_pipe[2];
    int out_pipe[2];

    if (pipe(in_pipe) == -1)
        throw std::runtime_error("500 pipe() failed");
    if (pipe(out_pipe) == -1) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        throw std::runtime_error("500 pipe() failed");
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        throw std::runtime_error("500 fork() failed");
    }

    // Child process
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);

        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);


        // Resolve het CGI binary pad (dat MOET bestaan)
        char abs_binary[4096];
        if (realpath(_cgi.binary.c_str(), abs_binary) == NULL) {
            write(STDOUT_FILENO, "Status: 500\r\n\r\n", 15);
            exit(1);
        }

        // Voor het script pad: als het bestaat gebruiken we absolute, anders relatief
        char abs_path[4096];
        bool script_exists = (realpath(_filepath.c_str(), abs_path) != NULL);
        if (!script_exists) {
            // Bestand bestaat niet, maar dat is OK voor sommige CGI binaries.
            // Kopieer gewoon het originele pad.
            std::strncpy(abs_path, _filepath.c_str(), sizeof(abs_path) - 1);
            abs_path[sizeof(abs_path) - 1] = '\0';
        }

        std::string dir = getScriptDir();
        // chdir mag falen (als de directory niet bestaat), dat is geen fout.
        (void)chdir(dir.c_str());

        std::vector<std::string> env_vec = buildEnv();
        char **env = envToCharpp(env_vec);

        char *args[3];
        args[0] = abs_binary;

        // Als binary == script (standalone CGI binary), geen argument meegeven
        if (std::strcmp(abs_binary, abs_path) == 0) {
            args[1] = NULL;
        } else {
            args[1] = abs_path;
            args[2] = NULL;
        }

        execve(abs_binary, args, env);

        freeCharpp(env);
        write(STDOUT_FILENO, "Status: 500\r\n\r\n", 15);
        exit(1);
    }

    // Parent process

    // Sluit uiteinden die de parent niet nodig heeft
    close(in_pipe[0]);
    close(out_pipe[1]);

    // Zet write pipe non-blocking voor async schrijven
    fcntl(in_pipe[1], F_SETFL, O_NONBLOCK);

    // Zet read pipe non-blocking zodat epoll correct werkt
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

    out_pid = pid;

    // Als er een body is, geef de write fd terug voor async schrijven
    // Anders sluit de write fd meteen (EOF signaal)
    if (!_req.body.empty()) {
        out_write_fd = in_pipe[1];
    } else {
        close(in_pipe[1]);
        out_write_fd = -1;
    }

    return out_pipe[0];  // Server registreert deze fd in epoll voor lezen
}

// parseCgiOutput(): parset ruwe CGI stdout naar een Response
// Formaat: headers\r\n\r\nbody  of  headers\n\nbody

Response CgiHandler::parseCgiOutput(const std::string &raw_output,
                                    const ServerConfig &server) {
    Response res;
    res.status_code = 200;
    res.status_msg  = "OK";

    if (raw_output.empty()) {
        res.status_code = 500;
        res.status_msg  = "Internal Server Error";
        res.setBody("<html><body><h1>CGI produced no output</h1></body></html>",
                    "text/html");
        return res;
    }

    // Zoek scheiding tussen headers en body
    size_t sep = raw_output.find("\r\n\r\n");
    size_t sep_len = 4;
    if (sep == std::string::npos) {
        sep = raw_output.find("\n\n");
        sep_len = 2;
    }

    if (sep == std::string::npos) {
        res.setBody(raw_output, "text/html");
        return res;
    }

    std::string header_section = raw_output.substr(0, sep);
    res.body = raw_output.substr(sep + sep_len);

    std::istringstream ss(header_section);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);

        size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;

        std::string key   = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        if (!value.empty() && value[0] == ' ')
            value.erase(0, 1);

        if (key == "Status") {
            res.status_code = std::atoi(value.c_str());
            // Probeer de status message uit het value te halen (bv. "404 Not Found")
            size_t sp = value.find(' ');
            if (sp != std::string::npos && sp + 1 < value.size()) {
                res.status_msg = value.substr(sp + 1);
            } else {
                // Geen message in de CGI output — gebruik standaard HTTP message
                ResponseBuilder rb;
                res.status_msg = rb.getStatusMessage(res.status_code);
            }
        } else {
            res.headers[key] = value;
        }
    }

    // Zorg dat er altijd een Content-Type is
    if (res.headers.find("Content-Type") == res.headers.end())
        res.headers["Content-Type"] = "text/html";

    (void)server;
    return res;
}

// Environment variables

std::vector<std::string> CgiHandler::buildEnv() const {
    std::vector<std::string> env;

    env.push_back("REQUEST_METHOD=" + _req.method);
    env.push_back("QUERY_STRING=" + _req.query);
    env.push_back("SCRIPT_FILENAME=" + _filepath);
    // SCRIPT_NAME weggelaten - veroorzaakt PATH_INFO incorrect error met cgi_tester
    env.push_back("PATH_INFO=" + _req.uri);
    env.push_back("SERVER_PROTOCOL=" + _req.version);
    env.push_back("SERVER_SOFTWARE=webserv/1.0");
    env.push_back("GATEWAY_INTERFACE=CGI/1.1");
    env.push_back("REDIRECT_STATUS=200");

    std::ostringstream cl;
    cl << _req.body.size();
    env.push_back("CONTENT_LENGTH=" + cl.str());

    if (_req.method == "POST") {
        std::string ct = _req.getHeader("content-type");
        if (!ct.empty())
            env.push_back("CONTENT_TYPE=" + ct);
    }

    for (std::map<std::string, std::string>::const_iterator it =
             _req.headers.begin(); it != _req.headers.end(); it++) {
        std::string key = "HTTP_";
        for (size_t i = 0; i < it->first.size(); i++) {
            char c = it->first[i];
            key += (c == '-') ? '_' : std::toupper(c);
        }
        env.push_back(key + "=" + it->second);
    }

    return env;
}

char **CgiHandler::envToCharpp(const std::vector<std::string> &env) const {
    char **result = new char*[env.size() + 1];
    for (size_t i = 0; i < env.size(); i++) {
        result[i] = new char[env[i].size() + 1];
        std::strcpy(result[i], env[i].c_str());
    }
    result[env.size()] = NULL;
    return result;
}

void CgiHandler::freeCharpp(char **arr) const {
    for (int i = 0; arr[i] != NULL; i++)
        delete[] arr[i];
    delete[] arr;
}

std::string CgiHandler::getScriptDir() const {
    size_t pos = _filepath.rfind('/');
    if (pos == std::string::npos)
        return ".";
    return _filepath.substr(0, pos);
}

std::string CgiHandler::getScriptName() const {
    size_t pos = _filepath.rfind('/');
    if (pos == std::string::npos)
        return _filepath;
    return _filepath.substr(pos + 1);
}