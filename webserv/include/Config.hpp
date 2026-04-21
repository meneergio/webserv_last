#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include <vector>
#include <map>

// CGI configuratie per extensie
struct CgiConfig {
    std::string extension;  // ".php"
    std::string binary;     // "/usr/bin/php-cgi"

    CgiConfig() {}
    CgiConfig(const std::string &ext, const std::string &bin)
        : extension(ext), binary(bin) {}
};

// Een location block (route)
struct Location {
    std::string path;                   // "/upload"
    std::string root;                   // "/var/www/html"
    std::string index;                  // "index.html"
    std::string redirect;               // "" of "http://..."
    std::string upload_dir;             // waar uploads naartoe
    std::vector<std::string> methods;   // ["GET", "POST"]
    std::vector<CgiConfig> cgi;
    bool directory_listing;
    size_t max_body_size;               // 0 = gebruik server default

    Location()
        : directory_listing(false)
        , max_body_size(0)
    {}

    bool methodAllowed(const std::string &method) const {
    for (size_t i = 0; i < methods.size(); i++) {
        if (methods[i] == method)
            return true;
    }
    return false;
    }
};

// Een server block
struct ServerConfig {
    std::string host;                           // "127.0.0.1"
    int port;                                   // 8080
    std::string server_name;                    // optioneel
    size_t max_body_size;                       // server-level default (bytes)
    std::map<int, std::string> error_pages;     // {404: "/errors/404.html"}
    std::vector<Location> locations;

    ServerConfig()
        : host("0.0.0.0")
        , port(80)
        , max_body_size(1048576)  // 1MB default
    {}

    // Zoek de meest specifieke location voor een gegeven URI
    const Location *matchLocation(const std::string &uri) const {
        const Location *best = NULL;
        size_t best_len = 0;
        for (size_t i = 0; i < locations.size(); i++) {
            const std::string &lpath = locations[i].path;
            if (uri.find(lpath) == 0) {
                if (lpath.size() > best_len) {
                    best_len = lpath.size();
                    best = &locations[i];
                }
            }
        }
        return best;
    }
};

#endif