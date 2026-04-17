#ifndef CONFIGPARSER_HPP
#define CONFIGPARSER_HPP

#include "Config.hpp"
#include <string>
#include <vector>
#include <stdexcept>

class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();

    // Hoofdfunctie: geeft een vector van ServerConfigs terug
    std::vector<ServerConfig> parse(const std::string &filename);

private:
    std::string             _content;   // volledige bestandsinhoud
    size_t                  _pos;       // huidige positie in _content

    // Leesfuncties
    void        loadFile(const std::string &filename);
    void        skipWhitespace();
    void        skipComment();
    std::string readWord();
    std::string readLine();
    void        expect(char c);

    // Blok parsers
    ServerConfig    parseServer();
    Location        parseLocation(const std::string &path);

    // Regel parsers
    void    parseServerDirective(ServerConfig &server);
    void    parseLocationDirective(Location &loc);
    void    parseCgi(Location &loc);
    void    parseErrorPage(ServerConfig &server);
    void    parseMethods(Location &loc);

    // Hulpfuncties
    size_t  parseSize(const std::string &value);
    bool    isEof() const;
};

#endif
