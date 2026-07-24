#pragma once

#include <string>

namespace analytics {

class KdbClient
{
public:
    KdbClient();
    ~KdbClient();

    bool connect(const std::string& host, int port);

    void disconnect();

    bool isConnected() const;

    bool execute(const std::string& query);

private:
    int handle_;
};

} // namespace analytics
