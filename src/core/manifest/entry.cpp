#include "core/manifest/entry.h"

#include <tuple>

namespace va {

bool canonical_less(const Entry& left, const Entry& right)
{
    const auto rank = [](char type) { return type == 's' ? 1 : 0; };
    // std::string compares like memcmp (unsigned bytes), which is the bytewise
    // UTF-8 order R-43 asks for.
    return std::tie(left.path.p, left.path.raw) < std::tie(right.path.p, right.path.raw) ||
           (std::tie(left.path.p, left.path.raw) == std::tie(right.path.p, right.path.raw) &&
               std::make_tuple(rank(left.type), left.stream) <
                   std::make_tuple(rank(right.type), right.stream));
}

std::string entry_key(const Entry& entry)
{
    std::string key = entry.path.p;
    key += '\0';
    key += entry.path.raw;
    key += '\0';
    key += entry.stream;
    return key;
}

} // namespace va
