#ifndef REQUEST_HPP
#define REQUEST_HPP

#include <string>
#include <map>

// Parse status: houdt bij hoe ver we zijn
enum ParseState {
    PARSE_REQUEST_LINE,  // eerste regel: METHOD URI VERSION
    PARSE_HEADERS,       // headers lezen
    PARSE_BODY,          // body lezen (POST)
    PARSE_CHUNKED,       // chunked transfer encoding
    PARSE_COMPLETE,      // volledig geparsed
    PARSE_ERROR          // ongeldige request
};

// Het Request object
// Je partner leest dit uit om een response te bouwen
struct Request {
    std::string                         method;     // "GET", "POST", "DELETE"
    std::string                         uri;        // "/index.html"
    std::string                         version;    // "HTTP/1.1"
    std::string                         query;      // "?foo=bar" deel van URI
    std::map<std::string, std::string>  headers;    // lowercase keys
    std::string                         body;

    ParseState  state;
    std::string error_msg;
    size_t      content_length;     // uit Content-Length header
    size_t      max_body_size;      // uit ServerConfig

    Request()
        : state(PARSE_REQUEST_LINE)
        , content_length(0)
        , max_body_size(1048576)
    {}

    void        reset();
    bool        isComplete() const  { return state == PARSE_COMPLETE; }
    bool        hasError() const    { return state == PARSE_ERROR; }
    bool        keepAlive() const;
    std::string getHeader(const std::string &key) const;
};

// De parser zelf
// Werkt incrementeel: kan meerdere keren geroepen worden
// met telkens meer data (zoals kqueue die aanlevert)
class RequestParser {
public:
    RequestParser();
    ~RequestParser();

    // Geef ruwe data mee, parser vult de Request aan
    // Geeft terug hoeveel bytes verbruikt werden
    size_t  parse(Request &req, const std::string &data);

    // Reset de interne buffer (voor keep-alive connections)
    void    reset() { _buffer.clear(); }

private:
    // Per fase een aparte functie
    bool    parseRequestLine(Request &req, std::string &buffer);
    bool    parseHeaders(Request &req, std::string &buffer);
    bool    parseBody(Request &req, std::string &buffer);
    bool    parseChunked(Request &req, std::string &buffer);

    // Hulpfuncties
    std::string toLower(const std::string &s) const;
    std::string trim(const std::string &s) const;
    bool        isValidMethod(const std::string &method) const;
    bool        isValidVersion(const std::string &version) const;
    void        setError(Request &req, const std::string &msg) const;

    std::string _buffer;    // interne buffer voor incomplete data
};

#endif