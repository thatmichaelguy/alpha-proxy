#include <iostream>
#include <thread>
#include "server.h"

int main(int argc, char **argv)
{
    int port = 7162;
    int maxConnectionsPerIP = 10;
    if (argc >= 2)
    {
        port = atoi(argv[1]);

        if (argc >= 3)
        {
            maxConnectionsPerIP = atoi(argv[2]);
        }
    }
    Server s(port, maxConnectionsPerIP);
    std::cout << "Starting proxy server on port " << port << " with a maximum of " << maxConnectionsPerIP << " connections per IP allowed" << std::endl;
    s.run(8);
    return 0;
}
