#pragma once
// A path, such as an archive root, written as one argument of a command that
// a message tells the operator to run ("continue with `register K:\Photos
// --resume`"), so that the command can be typed or copied as it is shown
// (decision 0022, amended 2026-10-27).
#include <string>
#include <string_view>

namespace va {

// `text`, a path in UTF-8, as it is when it holds only letters, digits,
// characters beyond ASCII and `\ / : . _ -`, which no shell reads otherwise.
// Anything else (a space, parentheses, `&`, `$`, ...) puts it in quotes: on
// Windows in double quotes, which the command prompt and PowerShell both take,
// with the backslashes it ends with doubled ("\\?\Volume{...}\\"), as the
// program's argument reader would otherwise take the last one with the
// closing quote as a quote of the path; on Linux in single quotes, inside
// which a shell reads nothing but the quote itself, written '\''. Empty text
// is an empty argument in quotes.
std::string shell_argument(std::string_view text);

} // namespace va
