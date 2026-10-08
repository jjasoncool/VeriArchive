#include "common/unicode.h"

#include "common/utf8.h"

#include <glib.h>

namespace va {
namespace {

// GLib returns newly allocated C strings; this copies one into std::string and
// frees it, so callers never have to remember g_free().
std::string take_glib_string(gchar* text)
{
    if (!text) {
        return {};
    }
    std::string result(text);
    g_free(text);
    return result;
}

} // namespace

std::string unicode_nfc(std::string_view utf8)
{
    const std::string valid = is_valid_utf8(utf8) ? std::string(utf8) : utf8_lossy(utf8);
    return take_glib_string(g_utf8_normalize(
        valid.data(), static_cast<gssize>(valid.size()), G_NORMALIZE_DEFAULT_COMPOSE));
}

std::string unicode_fold(std::string_view utf8)
{
    const auto normalised = unicode_nfc(utf8);
    const auto folded = take_glib_string(
        g_utf8_casefold(normalised.data(), static_cast<gssize>(normalised.size())));
    return unicode_nfc(folded);
}

} // namespace va
