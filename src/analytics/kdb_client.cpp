extern "C" {
#include "k.h"
}

#include "analytics/kdb_client.hpp"

namespace analytics {

KdbClient::KdbClient()
    : handle_(-1)
{
}

KdbClient::~KdbClient()
{
    disconnect();
}

bool KdbClient::connect(const std::string& host, int port)
{
    handle_ = khpu(const_cast<char*>(host.c_str()), port, const_cast<char*>(""));

    return handle_ >= 0;
}

void KdbClient::disconnect()
{
    if (handle_ >= 0)
    {
        kclose(handle_);
        handle_ = -1;
    }
}

bool KdbClient::isConnected() const
{
	
    return handle_ >= 0;
}

bool KdbClient::execute(const std::string& query)
{
    if (handle_ < 0)
        return false;

    K result = k(handle_,const_cast<char*> (query.c_str()), (K)0);

    if (!result)
        return false;

    r0(result);

    return true;
}

} // namespace analytics
