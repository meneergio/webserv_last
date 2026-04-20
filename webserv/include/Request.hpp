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
struct Request {
    std::string                         method;
    std::string                         uri;
    std::string                         version;
    std::string                         query;
    std::map<std::string, std::string>  headers;
    std::string                         body;

    ParseState  state;
    std::string error_msg;
    size_t      content_length;
    size_t      max_body_size;

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
class RequestParser {
public:
    RequestParser();
    ~RequestParser();

    size_t  parse(Request &req, const std::string &data);

    void               reset()     { _buffer.clear(); }
    const std::string &getBuffer() const { return _buffer; }  // voor pipelining

private:
    bool    parseRequestLine(Request &req, std::string &buffer);
    bool    parseHeaders(Request &req, std::string &buffer);
    bool    parseBody(Request &req, std::string &buffer);
    bool    parseChunked(Request &req, std::string &buffer);

    std::string toLower(const std::string &s) const;
    std::string trim(const std::string &s) const;
    bool        isValidMethod(const std::string &method) const;
    bool        isValidVersion(const std::string &version) const;
    void        setError(Request &req, const std::string &msg) const;

    std::string _buffer;
};

#endif