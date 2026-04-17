#include "../include/ConfigParser.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <cstdlib>


// Constructor / Destructor


ConfigParser::ConfigParser() : _pos(0) {}
ConfigParser::~ConfigParser() {}


// Hoofdfunctie


std::vector<ServerConfig> ConfigParser::parse(const std::string &filename) {
    loadFile(filename);
    std::vector<ServerConfig> servers;

    while (!isEof()) {
        skipWhitespace();
        if (isEof())
            break;
        std::string word = readWord();
        if (word == "server") {
            skipWhitespace();
            expect('{');
            servers.push_back(parseServer());
        } else {
            throw std::runtime_error("Unexpected token: " + word);
        }
    }

    if (servers.empty())
        throw std::runtime_error("Config file has no server blocks");

    // Check op dubbele host:port:server_name combinaties
    for (size_t i = 0; i < servers.size(); i++) {
        for (size_t j = i + 1; j < servers.size(); j++) {
            if (servers[i].host == servers[j].host
                && servers[i].port == servers[j].port
                && servers[i].server_name == servers[j].server_name) {
                std::ostringstream ss;
                ss << "Duplicate server combination: "
                   << servers[i].host << ":"
                   << servers[i].port;
                if (!servers[i].server_name.empty())
                    ss << " (" << servers[i].server_name << ")";
                throw std::runtime_error(ss.str());
            }
        }
    }

    return servers;
}


// Bestand inladen


void ConfigParser::loadFile(const std::string &filename) {
    std::ifstream file(filename.c_str());
    if (!file.is_open())
        throw std::runtime_error("Cannot open config file: " + filename);

    std::ostringstream ss;
    ss << file.rdbuf();
    _content = ss.str();
    _pos = 0;
}


// Leesfuncties


bool ConfigParser::isEof() const {
    return _pos >= _content.size();
}

void ConfigParser::skipWhitespace() {
    while (!isEof() && (std::isspace(_content[_pos]) || _content[_pos] == '#')) {
        if (_content[_pos] == '#')
            skipComment();
        else
            _pos++;
    }
}

void ConfigParser::skipComment() {
    while (!isEof() && _content[_pos] != '\n')
        _pos++;
}

// Leest één woord (tot whitespace of speciale tekens)
std::string ConfigParser::readWord() {
    skipWhitespace();
    std::string word;
    while (!isEof() && !std::isspace(_content[_pos])
           && _content[_pos] != '{' && _content[_pos] != '}'
           && _content[_pos] != ';') {
        word += _content[_pos++];
    }
    return word;
}

// Leest de rest van de regel tot ';'
std::string ConfigParser::readLine() {
    skipWhitespace();
    std::string line;
    while (!isEof() && _content[_pos] != ';' && _content[_pos] != '\n') {
        line += _content[_pos++];
    }
    // trim trailing whitespace
    size_t end = line.find_last_not_of(" \t");
    if (end != std::string::npos)
        line = line.substr(0, end + 1);
    return line;
}

void ConfigParser::expect(char c) {
    skipWhitespace();
    if (isEof() || _content[_pos] != c) {
        std::string msg = "Expected '";
        msg += c;
        msg += "'";
        throw std::runtime_error(msg);
    }
    _pos++;
}


// Server block parser


ServerConfig ConfigParser::parseServer() {
    ServerConfig server;

    while (!isEof()) {
        skipWhitespace();
        if (isEof())
            throw std::runtime_error("Unclosed server block");
        if (_content[_pos] == '}') {
            _pos++;
            break;
        }
        parseServerDirective(server);
    }
    return server;
}

void ConfigParser::parseServerDirective(ServerConfig &server) {
    std::string key = readWord();

    if (key == "host") {
        server.host = readWord();
        expect(';');
    } else if (key == "port") {
        std::string val = readWord();
        server.port = std::atoi(val.c_str());
        if (server.port <= 0 || server.port > 65535)
            throw std::runtime_error("Invalid port: " + val);
        expect(';');
    } else if (key == "server_name") {
        server.server_name = readWord();
        expect(';');
    } else if (key == "max_body_size") {
        std::string val = readWord();
        server.max_body_size = parseSize(val);
        expect(';');
    } else if (key == "error_page") {
        parseErrorPage(server);
    } else if (key == "location") {
        std::string path = readWord();
        skipWhitespace();
        expect('{');
        server.locations.push_back(parseLocation(path));
    } else {
        throw std::runtime_error("Unknown server directive: " + key);
    }
}


// Location block parser


Location ConfigParser::parseLocation(const std::string &path) {
    Location loc;
    loc.path = path;

    while (!isEof()) {
        skipWhitespace();
        if (isEof())
            throw std::runtime_error("Unclosed location block");
        if (_content[_pos] == '}') {
            _pos++;
            break;
        }
        parseLocationDirective(loc);
    }
    return loc;
}

void ConfigParser::parseLocationDirective(Location &loc) {
    std::string key = readWord();

    if (key == "root") {
        loc.root = readWord();
        expect(';');
    } else if (key == "index") {
        loc.index = readWord();
        expect(';');
    } else if (key == "methods") {
        parseMethods(loc);
    } else if (key == "directory_listing") {
        std::string val = readWord();
        loc.directory_listing = (val == "on");
        expect(';');
    } else if (key == "upload_dir") {
        loc.upload_dir = readWord();
        expect(';');
    } else if (key == "redirect") {
        loc.redirect = readWord();
        expect(';');
    } else if (key == "max_body_size") {
        std::string val = readWord();
        loc.max_body_size = parseSize(val);
        expect(';');
    } else if (key == "cgi") {
        parseCgi(loc);
    } else {
        throw std::runtime_error("Unknown location directive: " + key);
    }
}


// Specifieke directive parsers


void ConfigParser::parseErrorPage(ServerConfig &server) {
    std::string code_str = readWord();
    int code = std::atoi(code_str.c_str());
    if (code < 100 || code > 599)
        throw std::runtime_error("Invalid error code: " + code_str);
    std::string path = readWord();
    server.error_pages[code] = path;
    expect(';');
}

void ConfigParser::parseMethods(Location &loc) {
    loc.methods.clear();
    skipWhitespace();
    // Lees alle methodes tot ';'
    while (!isEof() && _content[_pos] != ';') {
        std::string method = readWord();
        if (!method.empty()) {
            if (method != "GET" && method != "POST" && method != "DELETE")
                throw std::runtime_error("Unknown HTTP method: " + method);
            loc.methods.push_back(method);
        }
        skipWhitespace();
    }
    expect(';');
}

void ConfigParser::parseCgi(Location &loc) {
    std::string ext = readWord();
    std::string bin = readWord();
    if (ext.empty() || bin.empty())
        throw std::runtime_error("Invalid cgi directive");
    loc.cgi.push_back(CgiConfig(ext, bin));
    expect(';');
}


// Hulpfuncties


// Converteert "1048576" of "1M" of "10K" naar bytes
size_t ConfigParser::parseSize(const std::string &value) {
    if (value.empty())
        throw std::runtime_error("Empty size value");

    size_t num = std::atoi(value.c_str());
    char last = value[value.size() - 1];

    if (last == 'K' || last == 'k')
        return num * 1024;
    if (last == 'M' || last == 'm')
        return num * 1024 * 1024;
    if (last == 'G' || last == 'g')
        return num * 1024 * 1024 * 1024;
    return num;
}
