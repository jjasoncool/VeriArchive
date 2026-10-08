#include "core/engine/sha256sums.h"

#include "common/hex.h"

namespace va {

Sha256Sums sha256sums(const std::vector<Entry>& entries)
{
    Sha256Sums sums;
    for (const auto& entry : entries) {
        if (entry.type != 'f') {
            continue;
        }
        if (entry.sha256.empty()) {
            sums.skipped.push_back(entry.path.p + ": no hash (unreadable at registration)");
            continue;
        }
        std::string name = entry.path.p;
        if (entry.path.enc == "bytes") {
            const auto bytes = from_hex(entry.path.raw);
            if (bytes) {
                name.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            }
        } else if (!entry.path.enc.empty()) {
            sums.skipped.push_back(entry.path.p + ": name is not valid Unicode (" + entry.path.enc + ")");
            continue;
        }

        const bool escape = name.find_first_of("\\\n") != std::string::npos;
        if (escape) {
            std::string escaped;
            for (const char character : name) {
                if (character == '\\') {
                    escaped += "\\\\";
                } else if (character == '\n') {
                    escaped += "\\n";
                } else {
                    escaped += character;
                }
            }
            name = std::move(escaped);
            sums.text += '\\';
        }
        sums.text += entry.sha256;
        sums.text += "  ";
        sums.text += name;
        sums.text += '\n';
    }
    return sums;
}

} // namespace va
