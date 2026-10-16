#include "platform/windows/long_path.h"

#include <algorithm>
#include <vector>

namespace va::platform::windows {
namespace {

bool is_separator(wchar_t character)
{
    return character == L'\\' || character == L'/';
}

// `rest`, what follows "C:" or "\\server\share" in a path: nothing, or a
// separator and the names below that root. Each run of separators becomes
// one '\', "." is dropped and ".." drops the name before it, never the root,
// as lexically_normal() does. The result ends with '\' when `rest` ends with
// a separator, "." or "..", and is "\" when no name is left.
std::wstring normal_rest(std::wstring_view rest)
{
    if (rest.empty()) {
        return {};
    }
    std::vector<std::wstring_view> names;
    bool ends_as_folder = false;
    std::size_t position = 0;
    while (position < rest.size()) {
        if (is_separator(rest[position])) {
            ends_as_folder = true;
            ++position;
            continue;
        }
        std::size_t end = position;
        while (end < rest.size() && !is_separator(rest[end])) {
            ++end;
        }
        const auto name = rest.substr(position, end - position);
        position = end;
        ends_as_folder = name == L"." || name == L"..";
        if (name == L"..") {
            if (!names.empty()) {
                names.pop_back();
            }
        } else if (name != L".") {
            names.push_back(name);
        }
    }
    std::wstring normal;
    for (const auto name : names) {
        normal += L'\\';
        normal += name;
    }
    if (names.empty() || ends_as_folder) {
        normal += L'\\';
    }
    return normal;
}

} // namespace

std::wstring long_path(std::wstring_view path)
{
    std::wstring text(path);
    std::replace(text.begin(), text.end(), L'/', L'\\');
    const std::wstring_view view(text);
    if (view.starts_with(LR"(\\?\)") || view.starts_with(LR"(\\.\)")) {
        return text;
    }
    if (view.starts_with(LR"(\\)")) {
        // A network path: the server, the share, then the names inside the share.
        const auto server_end = view.find(L'\\', 2);
        if (server_end == std::wstring_view::npos || server_end == 2) {
            return text;
        }
        const auto share_end = std::min(view.find(L'\\', server_end + 1), view.size());
        if (share_end == server_end + 1) {
            return text;
        }
        return LR"(\\?\UNC\)" + std::wstring(view.substr(2, share_end - 2)) +
               normal_rest(view.substr(share_end));
    }
    if (view.size() >= 3 && view[0] != L'\\' && view[1] == L':' && view[2] == L'\\') {
        return LR"(\\?\)" + std::wstring(view.substr(0, 2)) + normal_rest(view.substr(2));
    }
    return text;
}

} // namespace va::platform::windows
