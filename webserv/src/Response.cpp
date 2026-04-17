#include "../include/Response.hpp"
#include <sstream>
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <ctime>

static bool isSafeFilename(const std::string &filename) {
    if (filename.empty()) return false;
    if (filename.find("..") != std::string::npos) return false;
    if (filename.find('/') != std::string::npos) return false;
    if (filename.find('\\') != std::string::npos) return false;
    return true;
}

static std::string extractFilename(const std::string &disposition) {
    size_t pos = disposition.find("filename=\"");
    if (pos == std::string::npos) return "";
    pos += 10;
    size_t end = disposition.find("\"", pos);
    if (end == std::string::npos) return "";
    return disposition.substr(pos, end - pos);
}

std::string Response::serialize(const std::string &method) const {
    std::ostringstream raw;

    raw << "HTTP/1.1 " << status_code << " " << status_msg << "\r\n";

    // Date
    time_t now = time(NULL);
    char date_buf[128];
    struct tm *gmt = gmtime(&now);
    strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", gmt);
    raw << "Date: " << date_buf << "\r\n";

    raw << "Server: webserv/1.0\r\n";

    // 🔥 ALTIJD correcte Content-Length (GET gedrag)
    size_t body_len = body.size();

    // Eerst headers (zonder Content-Length)
    for (std::map<std::string, std::string>::const_iterator it = headers.begin();
         it != headers.end(); ++it) {
        if (it->first == "Content-Length")
            continue;
        raw << it->first << ": " << it->second << "\r\n";
    }

    // Dan Content-Length op het EINDE (zoals veel servers)
    raw << "Content-Length: " << body_len << "\r\n";

    raw << "\r\n";

    // 🔥 ENKEL verschil: body wel/niet sturen
    if (method != "HEAD")
        raw << body;

    return raw.str();
}

void Response::setHeader(const std::string &key, const std::string &value) {
    headers[key] = value;
}

void Response::setBody(const std::string &content, const std::string &content_type) {
    body = content;
    headers["Content-Type"] = content_type;
}

void Response::redirect(const std::string &location, int code) {
    status_code = code;
    status_msg  = (code == 301) ? "Moved Permanently" : "Found";
    headers["Location"] = location;
    body = "";
}

ResponseBuilder::ResponseBuilder() {}
ResponseBuilder::~ResponseBuilder() {}

Response ResponseBuilder::build(const Request &req,
                                const ServerConfig &server,
                                const Location *location) {
    if (!location)
        return serveErrorPage(404, server);

    if (!location->redirect.empty()) {
        Response res;
        res.redirect(location->redirect);
        return res;
    }

    // ✅ BELANGRIJKSTE FIX
    if (!location->methodAllowed(req.method))
        return serveErrorPage(405, server);

    if (req.method == "GET" || req.method == "HEAD")
        return handleGet(req, server, *location);

    if (req.method == "POST")
        return handlePost(req, server, *location);

    if (req.method == "DELETE")
        return handleDelete(req, *location);

    return serveErrorPage(501, server);
}

bool ResponseBuilder::isCgiRequest(const Request &req, const Location &loc) const {
    std::string filepath = resolvePath(loc, req.uri);
    return matchCgi(loc, filepath) != NULL;
}

Response ResponseBuilder::handleGet(const Request &req, const ServerConfig &server, const Location &loc) {
    std::string filepath = resolvePath(loc, req.uri);

    if (isDirectory(filepath)) {
        std::string index = filepath;
        if (index[index.size() - 1] != '/') index += '/';
        index += loc.index.empty() ? "index.html" : loc.index;

        if (fileExists(index))
            return serveStaticFile(index, server);
        if (loc.directory_listing)
            return serveDirectoryListing(filepath, req.uri);
        return serveErrorPage(404, server);
    }

    if (!fileExists(filepath))
        return serveErrorPage(404, server);

    if (matchCgi(loc, filepath))
        return makeCgiPlaceholder(filepath);

    return serveStaticFile(filepath, server);
}

Response ResponseBuilder::handlePost(const Request &req, const ServerConfig &server, const Location &loc) {
    std::string filepath = resolvePath(loc, req.uri);

    if (matchCgi(loc, filepath))
        return makeCgiPlaceholder(filepath);

    if (!loc.upload_dir.empty())
        return handleUpload(req, loc);

    return serveErrorPage(405, server);
}

Response ResponseBuilder::handleDelete(const Request &req, const Location &loc) {
    std::string filepath = resolvePath(loc, req.uri);

    if (!fileExists(filepath)) {
        Response res;
        res.status_code = 404;
        res.status_msg  = "Not Found";
        return res;
    }

    if (remove(filepath.c_str()) != 0) {
        Response res;
        res.status_code = 403;
        res.status_msg  = "Forbidden";
        return res;
    }

    Response res;
    res.status_code = 204;
    res.status_msg  = "No Content";
    return res;
}

Response ResponseBuilder::makeCgiPlaceholder(const std::string &filepath) const {
    Response res;
    res.status_code = 0;
    res.status_msg  = "CGI_PENDING";
    res.setHeader("X-CGI-Filepath", filepath);
    return res;
}

Response ResponseBuilder::serveStaticFile(const std::string &filepath, const ServerConfig &server) {
    std::ifstream file(filepath.c_str(), std::ios::binary);
    if (!file.is_open())
        return serveErrorPage(403, server);

    std::ostringstream ss;
    ss << file.rdbuf();

    Response res;
    res.status_code = 200;
    res.status_msg  = "OK";
    res.setBody(ss.str(), getMimeType(filepath));
    return res;
}

Response ResponseBuilder::serveDirectoryListing(const std::string &dirpath, const std::string &uri) {
    DIR *dir = opendir(dirpath.c_str());
    if (!dir) {
        Response res;
        res.status_code = 403;
        res.status_msg  = "Forbidden";
        return res;
    }

    std::ostringstream html;
    html << "<html><head><title>Index of " << uri << "</title></head>"
         << "<body><h1>Index of " << uri << "</h1><hr><pre>";

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string name = entry->d_name;
        if (name == ".") continue;
        html << "<a href=\"" << uri;
        if (uri[uri.size() - 1] != '/') html << "/";
        html << name << "\">" << name << "</a>\n";
    }
    closedir(dir);
    html << "</pre><hr></body></html>";

    Response res;
    res.status_code = 200;
    res.status_msg  = "OK";
    res.setBody(html.str(), "text/html");
    return res;
}

Response ResponseBuilder::handleUpload(const Request &req, const Location &loc) {
    std::string filename;
    std::string file_content;
    std::string content_type = req.getHeader("content-type");

    if (content_type.find("multipart/form-data") != std::string::npos) {
        size_t b_pos = content_type.find("boundary=");
        if (b_pos == std::string::npos) {
            Response res; res.status_code = 400; res.status_msg = "Bad Request"; return res;
        }
        std::string boundary = "--" + content_type.substr(b_pos + 9);
        size_t part_start = req.body.find(boundary);
        if (part_start == std::string::npos) {
            Response res; res.status_code = 400; res.status_msg = "Bad Request"; return res;
        }

        while (part_start != std::string::npos) {
            part_start += boundary.size();
            if (part_start >= req.body.size()) break;
            if (req.body.substr(part_start, 2) == "\r\n") part_start += 2;
            else if (req.body.substr(part_start, 2) == "--") break;
            size_t header_end = req.body.find("\r\n\r\n", part_start);
            if (header_end == std::string::npos) break;
            std::string part_headers = req.body.substr(part_start, header_end - part_start);
            size_t disp_pos = part_headers.find("Content-Disposition:");
            if (disp_pos != std::string::npos) {
                size_t disp_end = part_headers.find("\r\n", disp_pos);
                std::string disposition = part_headers.substr(disp_pos, disp_end - disp_pos);
                std::string fn = extractFilename(disposition);
                if (!fn.empty()) filename = fn;
            }
            size_t body_start = header_end + 4;
            size_t next_boundary = req.body.find("\r\n" + boundary, body_start);
            if (next_boundary == std::string::npos) break;
            if (!filename.empty()) {
                file_content = req.body.substr(body_start, next_boundary - body_start);
                break;
            }
            part_start = req.body.find(boundary, next_boundary);
        }
        if (filename.empty()) {
            Response res; res.status_code = 400; res.status_msg = "Bad Request: no file"; return res;
        }
    } else {
        file_content = req.body;
        std::string disposition = req.getHeader("content-disposition");
        if (!disposition.empty()) filename = extractFilename(disposition);
        if (filename.empty()) {
            std::ostringstream ss; ss << "upload_" << time(NULL); filename = ss.str();
        }
    }

    if (!isSafeFilename(filename)) {
        Response res;
        res.status_code = 400;
        res.status_msg  = "Bad Request";
        res.setBody("<html><body><h1>400 Bad Request</h1><p>Invalid filename.</p></body></html>", "text/html");
        return res;
    }

    std::string filepath = loc.upload_dir + "/" + filename;
    std::ofstream file(filepath.c_str(), std::ios::binary);
    if (!file.is_open()) {
        Response res; res.status_code = 500; res.status_msg = "Internal Server Error"; return res;
    }
    file.write(file_content.c_str(), file_content.size());
    file.close();

    Response res;
    res.status_code = 201;
    res.status_msg  = "Created";
    res.setHeader("Location", "/upload/" + filename);
    res.setBody("File uploaded: " + filename, "text/plain");
    return res;
}

Response ResponseBuilder::serveErrorPage(int code, const ServerConfig &server) {
    Response res;
    res.status_code = code;
    res.status_msg  = getStatusMessage(code);

    std::map<int, std::string>::const_iterator it = server.error_pages.find(code);
    if (it != server.error_pages.end()) {
        std::string filepath = (!server.locations.empty()) ? server.locations[0].root + it->second : it->second;
        std::ifstream file(filepath.c_str());
        if (file.is_open()) {
            std::ostringstream ss; ss << file.rdbuf();
            res.setBody(ss.str(), "text/html");
            return res;
        }
    }

    std::ostringstream body;
    body << "<html><head><title>" << code << " " << res.status_msg << "</title></head>"
         << "<body><h1>" << code << " " << res.status_msg << "</h1></body></html>";
    res.setBody(body.str(), "text/html");
    return res;
}

std::string ResponseBuilder::resolvePath(const Location &loc, const std::string &uri) const {
    std::string relative = uri.substr(loc.path.size());
    if (relative.empty() || relative[0] != '/') relative = "/" + relative;
    return loc.root + relative;
}

std::string ResponseBuilder::getMimeType(const std::string &filepath) const {
    size_t dot = filepath.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = filepath.substr(dot);
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".js") return "application/javascript";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".txt") return "text/plain";
    return "application/octet-stream";
}

std::string ResponseBuilder::getStatusMessage(int code) const {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

bool ResponseBuilder::fileExists(const std::string &path) const {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool ResponseBuilder::isDirectory(const std::string &path) const {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

const CgiConfig *ResponseBuilder::matchCgi(const Location &loc, const std::string &filepath) const {
    size_t dot = filepath.rfind('.');
    if (dot == std::string::npos) return NULL;
    std::string ext = filepath.substr(dot);
    for (size_t i = 0; i < loc.cgi.size(); i++) {
        if (loc.cgi[i].extension == ext) return &loc.cgi[i];
    }
    return NULL;
}