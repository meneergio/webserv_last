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

// Pas een binary-pad aan zodat het na een chdir naar `script_dir` nog
// correct verwijst naar dezelfde file.
//
// Voorbeeld: server cwd = X. Config zegt cgi binary = "./www/cgi-bin/foo".
// Het script ligt op "./www/YoupiBanane/youpi.bla" en we willen chdirren
// naar "./www/YoupiBanane" zodat de CGI relatieve files vindt.
// Na chdir is de cwd diepte 2 onder X, dus "./www/cgi-bin/foo" wordt
// "../../www/cgi-bin/foo".
//
// We gebruiken expres GEEN realpath/getcwd (niet in de toegelaten
// functielijst van het subject). Voor absolute paden (zoals
// "/usr/bin/python3") veranderen we niets.
static std::string adjustBinaryForChdir(const std::string &binary,
                                        const std::string &script_dir) {
    if (binary.empty() || binary[0] == '/')
        return binary;

    std::string clean = script_dir;
    if (clean.size() >= 2 && clean[0] == '.' && clean[1] == '/')
        clean = clean.substr(2);
    if (clean == "." || clean.empty())
        return binary;

    // Aantal directory-componenten in het chdir-pad
    int depth = 1;
    for (size_t i = 0; i < clean.size(); ++i)
        if (clean[i] == '/') ++depth;

    std::string prefix;
    for (int i = 0; i < depth; ++i)
        prefix += "../";

    std::string b = binary;
    if (b.size() >= 2 && b[0] == '.' && b[1] == '/')
        b = b.substr(2);
    return prefix + b;
}

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


        // Verifieer eerst dat het binary bereikbaar is vanaf de huidige cwd.
        // access() staat in de toegelaten functielijst van het subject.
        if (access(_cgi.binary.c_str(), X_OK) != 0) {
            write(STDOUT_FILENO, "Status: 500\r\n\r\n", 15);
            exit(1);
        }

        // Bereken een binary-pad dat OOK na chdir naar de script-directory
        // nog naar hetzelfde bestand wijst. Voor absolute paden is dit
        // ongewijzigd; voor relatieve paden voegen we ../-prefixen toe.
        std::string script_dir   = getScriptDir();
        std::string adjusted_bin = adjustBinaryForChdir(_cgi.binary, script_dir);

        // Het script-argument vereenvoudigen tot zijn basename: na chdir
        // staan we al in zijn directory.
        std::string adjusted_script = "./" + getScriptName();

        // chdir naar de script-directory zodat de CGI relatieve file-access
        // doet vanuit de script-locatie (zoals het subject vraagt).
        // chdir mag falen als de directory niet bestaat — geen fout: dan
        // executeren we vanaf de huidige cwd.
        (void)chdir(script_dir.c_str());

        std::vector<std::string> env_vec = buildEnv();
        char **env = envToCharpp(env_vec);

        char *args[3];
        args[0] = const_cast<char*>(adjusted_bin.c_str());

        // Als binary == script (standalone CGI binary), geen argument meegeven.
        if (std::strcmp(_cgi.binary.c_str(), _filepath.c_str()) == 0) {
            args[1] = NULL;
        } else {
            args[1] = const_cast<char*>(adjusted_script.c_str());
            args[2] = NULL;
        }

        execve(adjusted_bin.c_str(), args, env);

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

    // Open de write pipe altijd als er een body is of kan komen
    // (Content-Length > 0 of chunked). Bij early CGI start is
    // _req.body op dit moment nog leeg maar komt wel streamend binnen.
    bool expect_body = !_req.body.empty()
                    || _req.content_length > 0
                    || _req.getHeader("transfer-encoding") == "chunked";
    if (expect_body) {
        out_write_fd = in_pipe[1];
    } else {
        close(in_pipe[1]);
        out_write_fd = -1;
    }

    return out_pipe[0];
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

    // Bij early CGI start is _req.body nog leeg maar content_length > 0,
    // dus we gebruiken content_length wanneer dat hoger is.
    size_t cl_value = _req.body.size();
    if (_req.content_length > cl_value)
        cl_value = _req.content_length;
    std::ostringstream cl;
    cl << cl_value;
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