#include "../include/Request.hpp"
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <cstdlib>

// Request methodes

void Request::reset() {
    method.clear();
    uri.clear();
    version.clear();
    query.clear();
    headers.clear();
    body.clear();
    state          = PARSE_REQUEST_LINE;
    error_msg.clear();
    content_length = 0;
}

bool Request::keepAlive() const {
    std::map<std::string, std::string>::const_iterator it =
        headers.find("connection");
    if (it != headers.end()) {
        // Case-insensitive vergelijking van de header value
        std::string value = it->second;
        for (size_t i = 0; i < value.size(); i++)
            value[i] = std::tolower(value[i]);
        return value == "keep-alive";
    }
    // HTTP/1.1 is standaard keep-alive, HTTP/1.0 niet
    return version == "HTTP/1.1";
}

std::string Request::getHeader(const std::string &key) const {
    // Headers worden lowercase opgeslagen
    std::string lower_key;
    for (size_t i = 0; i < key.size(); i++)
        lower_key += std::tolower(key[i]);

    std::map<std::string, std::string>::const_iterator it =
        headers.find(lower_key);
    if (it != headers.end())
        return it->second;
    return "";
}

// RequestParser constructor/destructor

RequestParser::RequestParser() {}
RequestParser::~RequestParser() {}

// Hoofdfunctie: roep aan met nieuwe data

size_t RequestParser::parse(Request &req, const std::string &data) {
    _buffer += data;

    // Loop door de fases
    bool progress = true;
    while (progress && !req.isComplete() && !req.hasError()) {
        progress = false;

        if (req.state == PARSE_REQUEST_LINE)
            progress = parseRequestLine(req, _buffer);
        else if (req.state == PARSE_HEADERS)
            progress = parseHeaders(req, _buffer);
        else if (req.state == PARSE_BODY)
            progress = parseBody(req, _buffer);
        else if (req.state == PARSE_CHUNKED)
            progress = parseChunked(req, _buffer);
    }

    // Hoeveel hebben we verbruikt?
    size_t consumed = data.size() - (_buffer.size() > data.size()
        ? 0 : data.size() - _buffer.size());
    (void)consumed;
    return data.size(); // buffer beheert intern
}

// Fase 1: REQUEST LINE
// Voorbeeld: "GET /index.html HTTP/1.1\r\n"

bool RequestParser::parseRequestLine(Request &req, std::string &buffer) {
    size_t pos = buffer.find("\r\n");
    if (pos == std::string::npos)
        return false;  // nog niet genoeg data

    std::string line = buffer.substr(0, pos);
    buffer.erase(0, pos + 2);

    std::istringstream ss(line);
    if (!(ss >> req.method >> req.uri >> req.version)) {
        setError(req, "400 Bad Request: invalid request line");
        return false;
    }

    // Valideer method en version
    if (!isValidMethod(req.method)) {
        setError(req, "501 Not Implemented: unknown method");
        return false;
    }
    if (!isValidVersion(req.version)) {
        setError(req, "505 HTTP Version Not Supported");
        return false;
    }

    // Splits URI en query string
    size_t q = req.uri.find('?');
    if (q != std::string::npos) {
        req.query = req.uri.substr(q + 1);
        req.uri   = req.uri.substr(0, q);
    }

    req.state = PARSE_HEADERS;
    return true;
}

// Fase 2: HEADERS
// Elke header: "Key: Value\r\n"
// Lege regel "\r\n" = einde headers

bool RequestParser::parseHeaders(Request &req, std::string &buffer) {
    while (true) {
        size_t pos = buffer.find("\r\n");
        if (pos == std::string::npos)
            return false;  // wachten op meer data

        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 2);

        // Lege regel = einde van headers
        if (line.empty()) {
            // Bepaal wat de volgende fase is
            std::string transfer = req.getHeader("transfer-encoding");
            std::string cl       = req.getHeader("content-length");

            if (transfer == "chunked") {
                req.state = PARSE_CHUNKED;
            } else if (!cl.empty()) {
                req.content_length = static_cast<size_t>(std::atoi(cl.c_str()));
                if (req.content_length > req.max_body_size) {
                    setError(req, "413 Request Entity Too Large");
                    return false;
                }
                req.state = (req.content_length > 0) ? PARSE_BODY : PARSE_COMPLETE;
            } else {
                // Geen body
                req.state = PARSE_COMPLETE;
            }
            return true;
        }

        // Parse "Key: Value"
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            setError(req, "400 Bad Request: invalid header");
            return false;
        }

        std::string key   = toLower(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));

        if (key.empty()) {
            setError(req, "400 Bad Request: empty header key");
            return false;
        }

        req.headers[key] = value;
    }
}

// Fase 3: BODY (Content-Length)

bool RequestParser::parseBody(Request &req, std::string &buffer) {
    if (buffer.size() < req.content_length)
        return false;  // nog niet genoeg data

    req.body  = buffer.substr(0, req.content_length);
    buffer.erase(0, req.content_length);
    req.state = PARSE_COMPLETE;
    return true;
}

// Fase 4: CHUNKED TRANSFER ENCODING
// Formaat per chunk:
//   <hex grootte>\r\n
//   <data>\r\n
// Laatste chunk:
//   0\r\n
//   \r\n

bool RequestParser::parseChunked(Request &req, std::string &buffer) {
    while (true) {
        // Lees de chunk-grootte (hex getal op eerste regel)
        size_t pos = buffer.find("\r\n");
        if (pos == std::string::npos)
            return false;

        std::string size_line = buffer.substr(0, pos);
        size_t chunk_size = static_cast<size_t>(std::strtol(size_line.c_str(), NULL, 16));

        // Chunk grootte 0 = laatste chunk
        if (chunk_size == 0) {
            // Check of we genoeg data hebben voor "0\r\n" + trailing "\r\n"
            if (buffer.size() < pos + 4)  // pos + 2 voor "0\r\n", + 2 voor trailing "\r\n"
                return false;  // Wacht op meer data
            
            buffer.erase(0, pos + 4);  // Erase alles: "0\r\n\r\n"
            req.state = PARSE_COMPLETE;
            return true;
        }

        // Check max body size
        if (req.body.size() + chunk_size > req.max_body_size) {
            setError(req, "413 Request Entity Too Large");
            return false;
        }

        // Genoeg data voor deze chunk?
        if (buffer.size() < pos + 2 + chunk_size + 2)
            return false;

        // Chunk data toevoegen
        req.body += buffer.substr(pos + 2, chunk_size);
        buffer.erase(0, pos + 2 + chunk_size + 2);  // +2 voor \r\n na data
    }
}

// Hulpfuncties

std::string RequestParser::toLower(const std::string &s) const {
    std::string result;
    for (size_t i = 0; i < s.size(); i++)
        result += static_cast<char>(std::tolower(s[i]));
    return result;
}

std::string RequestParser::trim(const std::string &s) const {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

bool RequestParser::isValidMethod(const std::string &method) const {
    return method == "GET"
        || method == "POST"
        || method == "DELETE"
        || method == "HEAD"
        || method == "PUT";
    // HEAD en PUT zijn niet verplicht maar mogen niet crashen
}

bool RequestParser::isValidVersion(const std::string &version) const {
    return version == "HTTP/1.0" || version == "HTTP/1.1";
}

void RequestParser::setError(Request &req, const std::string &msg) const {
    req.state     = PARSE_ERROR;
    req.error_msg = msg;
}