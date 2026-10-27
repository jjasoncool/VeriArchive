#pragma once
// Paths the operator types, on the command line or in the GUI, made into the
// paths the engines receive. Both user interfaces make them here, so a folder
// typed in either is the same root: one root_hint, one root in plans and runs.
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace va::app {

// A path typed by the operator, in UTF-8: a relative path is taken relative to
// the working directory, as shells do, "." and ".." are resolved, and on
// Windows every '/' becomes '\' (lexically_normal). On Windows a path that
// starts with two separators, a network path ("\\server\share\dir") or one
// such as "\\?\D:\Photos", is taken as typed, because the C++ library reads
// it as a path on the current drive; but a volume's GUID path in that form
// ("\\?\Volume{...}\Photos\..\Music") is made so after its GUID, and its
// prefix written with '\' (platform::normal_volume_guid_path), as Windows
// reads that form as given (decision 0029, point 3). Empty text gives an
// empty path.
std::filesystem::path typed_path(std::string_view text);

// The folder or volume root typed by the operator (register, verify --full,
// archive relocate; the Register and Verify views): a typed_path without the
// separators it ends with ("D:\Photos\" is the folder "D:\Photos", while
// "D:\" and a volume's GUID path "\\?\Volume{...}\" stay), through a drive
// letter made with subst resolved to the folder that letter stands for, whose
// volume can be identified (decision 0027).
std::filesystem::path typed_root(std::string_view text);

// Why a root made by typed_root is refused before anything of it is read
// (decision 0029), in English for Text; nothing when it is not. VeriArchive
// registers and verifies folders on volumes attached to this computer: a root
// on a network share is refused, and so is one written in the \\?\ or the
// \\.\ form, whose message names the prefix as it was typed, but for a
// volume's GUID path in the \\?\ form ("\\?\Volume{...}\Photos"), the only
// name of a volume that has no drive letter or folder path, which is taken.
// One that names a volume's GUID otherwise ("\\.\Volume{...}\", a GUID cut
// short) is refused with words of its own. Each user interface asks this
// where the operator gives a root, before its volume is looked up; never of a
// root that VeriArchive finds itself (an archive verified by its uuid).
std::optional<std::string> root_refusal(const std::filesystem::path& root);

// Why a root made by typed_root from `typed` is not there, before anything
// of it is read (decision 0029, point 7): "the folder or volume <typed> was
// not found", and on Windows, where nothing is attached as its drive letter,
// "...: no volume is attached as K:; check that the device that holds it is
// attached". Nothing when it is there, or when that cannot be told (the
// volume lookup reports it then).
std::optional<std::string> root_absent(const std::filesystem::path& root, std::string_view typed);

// Why the root of `register`, `archive relocate` or `verify --full`, made by
// typed_root from `typed`, is not taken, before anything of it is read:
// refused (root_refusal), not there (root_absent), or no folder, "not a
// directory: <typed>", the root named as it was typed. Nothing when it is
// taken. The command line asks it of the root of these commands, the GUI of
// the root of Register Archive and Verify Archive, and the Archive List's
// Resume of a stopped registration of the root its plan names, as `register
// <root> --resume` asks it (decision 0022, point 7).
std::optional<std::string> root_not_taken(const std::filesystem::path& root, std::string_view typed);

} // namespace va::app
