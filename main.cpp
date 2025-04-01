#include <iostream>
#include <thread>
#include "server.h"
//#define DEBUG
//#define WITH_IP_EXTENSION
//#define ALLOW_ONLY_HAPROXY
//#define FREE_VERSION

int main(int argc, char** argv)
{
    int port = 7162;
    if(argc == 2) {
        port = atoi(argv[1]);
    }
    Server s(port);
    std::cout << "Starting proxy server on port " << port << std::endl;
    s.run(8);
    return 0;
}
