#pragma once

#include "PersistableTelemetry.hpp"

#include <string>

class LineProtocolSerializer
{
public:
    std::string serialize(const PersistableTelemetry &value) const;

private:
    static std::string escapeTagValue(const std::string &value);
    static std::string formatFloat(double value);
};
