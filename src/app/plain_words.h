#pragma once
// Plain words for what VeriArchive records as codes, for people to read
// (decision 0017, amended 2026-10-27): an item's detail and a run's failure
// class, and a count in the singular for one. The repository and --json keep
// the codes as they are; these give the English, which Text translates.
#include "app/text.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace va::app {

// An item's detail in plain English: a change of type ("d -> f") as "folder
// -> file", the metadata that differs ("mtime,mode") as "modification time,
// permissions", "link-target-translated" as "link target converted by this
// system", "known-since-registration" as "unreadable since registration",
// "name cannot exist on windows" as "name cannot exist on Windows". A detail
// that is a sentence already, or a system's message, as it is.
std::string item_detail_words(std::string_view detail);

// A run's failure class in plain English ("ManifestHashMismatch": "the
// manifest is not the one recorded"); an unknown class as it is.
std::string failure_class_words(std::string_view failure_class);

// `message` without the failure class that leads it in --json ("RepositoryError:
// no archive X" is "no archive X"); as it is where none leads it.
std::string_view without_failure_class(std::string_view message);

// The failure classes of a run as it records them, separated by ','
// ("ManifestHashMismatch,MediaIdentityMismatch"), each in plain words in the
// language of `text`, separated by "; ".
std::string failure_classes_words(const Text& text, std::string_view classes);

// What is counted.
enum class Counted { File, EmptyDirectory, Link, Byte, Entry, Archive, Revision };

// `count` of `counted` in the language of `text`, singular for one: "1 file",
// "2 files", "1 empty directory".
std::string count_of(const Text& text, std::uint64_t count, Counted counted);

} // namespace va::app
