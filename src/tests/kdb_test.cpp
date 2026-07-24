#include <iostream>

#include "analytics/kdb_client.hpp"

int main()
{
    analytics::KdbClient client;

    if (!client.connect("localhost", 5001))
    {
        std::cerr << "Failed to connect to q\n";
        return 1;
    }

    std::cout << "Connected to q!\n";

    client.execute("show \"Hello from C++\"");

    client.disconnect();

    return 0;
}
