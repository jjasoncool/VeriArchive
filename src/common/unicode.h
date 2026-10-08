#pragma once
// Unicode comparison helpers (decision 0013). VeriArchive never changes stored
// names (R-42); these produce comparison keys only:
//  - NFC: "é" typed as one code point and as "e" + combining accent compare equal.
//  - case folding: "Backups" and "backups" compare equal, as on NTFS/exFAT.
#include <string>
#include <string_view>

namespace va {

// NFC normalisation of valid UTF-8 (invalid input is first converted lossily).
std::string unicode_nfc(std::string_view utf8);

// Case-folded NFC key: equal keys mean "same name on a case-insensitive filesystem".
std::string unicode_fold(std::string_view utf8);

} // namespace va
