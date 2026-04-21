#ifndef RESPONSE_HPP
#define RESPONSE_HPP

#include "Config.hpp"
#include "Request.hpp"
#include <string>
#include <map>

struct Response {
    int                                 status_code;
    std::string                         status_msg;
    std::map<std::string, std::string>  headers;
    std::string                         body;

    Response() : status_code(200), status_msg("OK") {}

    std::string serialize(const std::string &method) const;

    void setHeader(const std::string &key, const std::string &value);
    void setBody(const std::string &content, const std::string &content_type);
    void redirect(const std::string &location, int code = 301);

    bool isCgiPending() const { return status_code == 0; }

    std::string getCgiFilepath() const {
        std::map<std::string, std::string>::const_iterator it = headers.find("X-CGI-Filepath");
        if (it != headers.end())
            return it->second;
        return "";
    }
};

class ResponseBuilder {
public:
    ResponseBuilder();
    ~ResponseBuilder();

    Response build(const Request &req,
                   const ServerConfig &server,
                   const Location *location);

    Response serveErrorPage(int code,
                            const ServerConfig &server);

    bool isCgiRequest(const Request &req, const Location &loc) const;
    std::string getStatusMessage(int code) const;

private:
    Response    handleGet(const Request &req,
                          const ServerConfig &server,
                          const Location &loc);
    Response    handlePost(const Request &req,
                           const ServerConfig &server,
                           const Location &loc);
    Response    handleDelete(const Request &req,
                             const Location &loc);

    Response    serveStaticFile(const std::string &filepath,
                                const ServerConfig &server);
    Response    serveDirectoryListing(const std::string &dirpath,
                                      const std::string &uri);
    Response    handleUpload(const Request &req,
                             const Location &loc);
    Response    makeCgiPlaceholder(const std::string &filepath) const;

    std::string resolvePath(const Location &loc,
                            const std::string &uri) const;
    std::string getMimeType(const std::string &filepath) const;
    bool        fileExists(const std::string &path) const;
    bool        isDirectory(const std::string &path) const;
    const CgiConfig *matchCgi(const Location &loc,
                               const std::string &filepath) const;
};

#endif