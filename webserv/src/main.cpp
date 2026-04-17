#include "../include/ConfigParser.hpp"
#include "../include/Server.hpp"
#include <iostream>
#include <csignal>
#include <cstdlib>

static void signalHandler(int sig) {
    (void)sig;
    std::cout << "\nShutting down..." << std::endl;
    exit(0);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: ./webserv <config_file>" << std::endl;
        return 1;
    }

    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN);  // voorkom crash bij broken pipe

    try {
        ConfigParser parser;
        std::vector<ServerConfig> configs = parser.parse(argv[1]);

        Server server(configs);
        server.run();

    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
