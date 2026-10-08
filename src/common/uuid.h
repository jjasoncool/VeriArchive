#pragma once
// UUIDs identify repositories and archives (R-33). VeriArchive writes them in the
// canonical lowercase form 8-4-4-4-12, e.g. 28ff645e-a220-4aac-ae0b-6c328bf1a2b3.
#include <string>
#include <string_view>

namespace va {

// Random (version 4) UUID.
std::string generate_uuid();

// True only for the canonical lowercase 8-4-4-4-12 hexadecimal form.
bool is_canonical_uuid(std::string_view text);

} // namespace va
