#pragma once
// Paths the operator types, on the command line or in the GUI, made into the
// paths the engines receive. Both user interfaces make them here, so a folder
// typed in either is the same root: one root_hint, one root in plans and runs.
#include <filesystem>
#include <string_view>

namespace va::app {

// A path typed by the operator, in UTF-8: a relative path is taken relative to
// the working directory, as shells do, "." and ".." are resolved, and on
// Windows every '/' becomes '\' (lexically_normal). On Windows a path that
// starts with two separators, a network path ("\\server\share\dir") or one
// such as "\\?\D:\Photos", is taken as typed, because the C++ library reads
// it as a path on the current drive. Empty text gives an empty path.
std::filesystem::path typed_path(std::string_view text);

// The folder or volume root typed by the operator (register, verify --full,
// archive relocate; the Register and Verify views): a typed_path without the
// separators it ends with ("D:\Photos\" is the folder "D:\Photos", while
// "D:\" stays), through a drive letter made with subst resolved to the folder
// that letter stands for, whose volume can be identified (decision 0027).
std::filesystem::path typed_root(std::string_view text);

} // namespace va::app
