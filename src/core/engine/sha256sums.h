#pragma once
// GNU `sha256sum -c` compatible export (R-67), so an archive can be checked
// without VeriArchive. Paths are relative to the archive root.
#include "core/manifest/entry.h"

#include <string>
#include <vector>

namespace va {

struct Sha256Sums {
    std::string text;                   // one "<hex>  <path>" line per file
    std::vector<std::string> skipped;   // files that cannot be listed, with the reason
};

// Names containing '\' or a newline use GNU's escaped form: the line starts
// with '\' and those characters are written as "\\" and "\n". Linux names
// recorded as raw bytes are written as those bytes; Windows raw names
// (unpaired surrogates) cannot be written to a text file and are skipped, as
// are files without a hash (unreadable at registration).
Sha256Sums sha256sums(const std::vector<Entry>& entries);

} // namespace va
