// What VeriArchive records as codes is read in plain words, and its messages
// name no decision, requirement or failure class (decision 0017, amended
// 2026-10-27); --json keeps the codes.
#include "test_support.h"

#include "app/plain_words.h"
#include "app/text.h"

#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "platform/windows/win_util.h"
#endif

namespace {

using va::app::item_detail_words;

// An item's detail: a change of type, the metadata that differs and the
// codes in plain words, in English and translated; a sentence or a system's
// message as it is.
void item_details_are_plain_words()
{
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    CHECK(item_detail_words("d -> f") == "folder -> file");
    CHECK(item_detail_words("f -> d") == "file -> folder");
    CHECK(item_detail_words("f -> l") == "file -> link");
    CHECK(item_detail_words("l -> d") == "link -> folder");
    CHECK(chinese(item_detail_words("d -> f")) == "資料夾 → 檔案");
    CHECK(chinese(item_detail_words("l -> f")) == "連結 → 檔案");
    CHECK(item_detail_words("mtime") == "modification time");
    CHECK(item_detail_words("mtime,mode,attrs") == "modification time, permissions, attributes");
    CHECK(chinese(item_detail_words("mode,attrs")) == "權限模式、檔案屬性");
    CHECK(item_detail_words("link-target-translated") == "link target converted by this system");
    CHECK(chinese(item_detail_words("link-target-translated")) == "連結目標已被轉換");
    CHECK(item_detail_words("known-since-registration") == "unreadable since registration");
    CHECK(chinese(item_detail_words("known-since-registration")) == "註冊時即無法讀取");
    CHECK(item_detail_words("name cannot exist on windows") == "name cannot exist on Windows");
    CHECK(chinese(item_detail_words("name cannot exist on linux")) == "此名稱無法存在於 Linux");
    for (const char* sentence : {"content differs", "size differs", "mtime,bogus", "x -> f", "",
             "not found (ReadFile, Windows error 2)"}) {
        CHECK(item_detail_words(sentence) == sentence);
    }
}

// A run's failure classes in plain words, translated; a message without the
// failure class that leads it in --json.
void failure_classes_are_plain_words()
{
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    CHECK(va::app::failure_class_words("ManifestHashMismatch") == "the manifest is not the one recorded");
    CHECK(va::app::failure_class_words("RepositoryError") == "the repository could not be read or written");
    CHECK(va::app::failure_class_words("SomethingNew") == "SomethingNew");
    for (const char* failure_class : {"MediaIdentityMismatch", "MediaIdentityAmbiguous", "ManifestSignatureFailure",
             "ManifestHashMismatch", "ManifestRevisionRollback", "ChangeTrackerContinuityLost",
             "FilesystemAccessError", "RepositoryError"}) {
        const auto words = va::app::failure_class_words(failure_class);
        CHECK(words != failure_class && chinese(words) != words);
    }
    CHECK(va::app::failure_classes_words(english, "ManifestHashMismatch,MediaIdentityMismatch") ==
          "the manifest is not the one recorded; the media is not the one registered");
    CHECK(va::app::failure_classes_words(chinese, "FilesystemAccessError") == "有檔案無法讀取");
    CHECK(va::app::failure_classes_words(english, "").empty());
    CHECK(va::app::without_failure_class("RepositoryError: archive c0ffee00 has no revision yet") ==
          "archive c0ffee00 has no revision yet");
    CHECK(va::app::without_failure_class("ManifestHashMismatch: the manifest changed while it was imported") ==
          "the manifest changed while it was imported");
    for (const char* message : {"no archive c0ffee00", "RepositoryError", "error: RepositoryError: x"}) {
        CHECK(va::app::without_failure_class(message) == message);
    }
}

// A count of one is said in the singular, in English; Chinese has no plural.
void one_is_said_in_the_singular()
{
    using va::app::count_of;
    using va::app::Counted;
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    CHECK(count_of(english, 1, Counted::File) == "1 file" && count_of(english, 0, Counted::File) == "0 files" &&
          count_of(english, 4, Counted::File) == "4 files");
    CHECK(count_of(english, 1, Counted::EmptyDirectory) == "1 empty directory" &&
          count_of(english, 2, Counted::EmptyDirectory) == "2 empty directories");
    CHECK(count_of(english, 1, Counted::Link) == "1 link" && count_of(english, 3, Counted::Link) == "3 links");
    CHECK(count_of(english, 1, Counted::Byte) == "1 byte" && count_of(english, 11, Counted::Byte) == "11 bytes");
    CHECK(count_of(english, 1, Counted::Entry) == "1 entry" && count_of(english, 21, Counted::Entry) == "21 entries");
    CHECK(count_of(english, 1, Counted::Archive) == "1 archive" && count_of(english, 2, Counted::Archive) == "2 archives");
    CHECK(count_of(english, 1, Counted::Revision) == "1 revision" &&
          count_of(english, 2, Counted::Revision) == "2 revisions");
    CHECK(count_of(chinese, 1, Counted::EmptyDirectory) == "1 個空資料夾" &&
          count_of(chinese, 2, Counted::EmptyDirectory) == "2 個空資料夾");
    CHECK(count_of(chinese, 1, Counted::Byte) == "1 位元組" && count_of(chinese, 2, Counted::Revision) == "2 個版次");
    CHECK(english.format("content: {}, {}, {}, {}", count_of(english, 4, Counted::File),
              count_of(english, 1, Counted::EmptyDirectory), count_of(english, 0, Counted::Link),
              count_of(english, 1, Counted::Byte)) == "content: 4 files, 1 empty directory, 0 links, 1 byte");
    CHECK(chinese.format("content: {}, {}, {}, {}", count_of(chinese, 4, Counted::File),
              count_of(chinese, 1, Counted::EmptyDirectory), count_of(chinese, 0, Counted::Link),
              count_of(chinese, 1, Counted::Byte)) == "內容：4 個檔案、1 個空資料夾、0 個連結、1 位元組");
}

#ifdef _WIN32
// The errors Windows reports most say what went wrong first, then the call
// and Windows' number; others name the number alone. Translated.
void windows_errors_are_plain_words()
{
    using va::platform::windows::error_from_win32;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    CHECK(error_from_win32(ERROR_ACCESS_DENIED, "CreateFileW").message == "access denied (CreateFileW, Windows error 5)");
    CHECK(error_from_win32(ERROR_FILE_NOT_FOUND, "CreateFileW").message == "not found (CreateFileW, Windows error 2)");
    CHECK(error_from_win32(ERROR_PATH_NOT_FOUND, "FindFirstFileExW").message ==
          "not found (FindFirstFileExW, Windows error 3)");
    CHECK(error_from_win32(ERROR_SHARING_VIOLATION, "CreateFileW").message ==
          "in use by another program (CreateFileW, Windows error 32)");
    CHECK(error_from_win32(ERROR_DISK_FULL, "WriteFile").message == "the volume is full (WriteFile, Windows error 112)");
    CHECK(error_from_win32(ERROR_CRC, "ReadFile").message ==
          "the device reported an error (ReadFile, Windows error 23)");
    // Not "cancelled": nobody cancelled anything, Windows aborted the request.
    CHECK(error_from_win32(ERROR_OPERATION_ABORTED, "ReadFile").message ==
          "aborted before it finished (ReadFile, Windows error 995)");
    CHECK(chinese(error_from_win32(ERROR_OPERATION_ABORTED, "ReadFile").message) ==
          "完成前即被中止（ReadFile，Windows 錯誤 995）");
    CHECK(error_from_win32(1234, "CreateFileW").message == "Windows error 1234 (CreateFileW)");
    CHECK(error_from_win32(ERROR_INVALID_DRIVE, "GetVolumePathNameW").message ==
          "no such drive letter (GetVolumePathNameW, Windows error 15)");
    CHECK(chinese("cannot scan the root: " + error_from_win32(ERROR_ACCESS_DENIED, "FindFirstFileExW").message) ==
          "無法掃描根目錄：存取被拒（FindFirstFileExW，Windows 錯誤 5）");
    CHECK(chinese(error_from_win32(ERROR_FILE_NOT_FOUND, "CreateFileW").message) ==
          "找不到（CreateFileW，Windows 錯誤 2）");
    CHECK(chinese(error_from_win32(1234, "CreateFileW").message) == "Windows 錯誤 1234（CreateFileW）");
    // Every plain word has its translation.
    for (const DWORD code : {ERROR_FILE_NOT_FOUND, ERROR_ACCESS_DENIED, ERROR_INVALID_DRIVE, ERROR_WRITE_PROTECT,
             ERROR_NOT_READY, ERROR_CRC, ERROR_FILE_CORRUPT, ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION,
             ERROR_DISK_FULL, ERROR_BAD_NETPATH, ERROR_ALREADY_EXISTS, ERROR_INVALID_NAME,
             ERROR_FILENAME_EXCED_RANGE, ERROR_DIRECTORY, ERROR_DIR_NOT_EMPTY, ERROR_DEVICE_NOT_CONNECTED,
             ERROR_PRIVILEGE_NOT_HELD, ERROR_SEM_TIMEOUT, ERROR_NOT_SUPPORTED, ERROR_OPERATION_ABORTED}) {
        const auto message = error_from_win32(code, "ReadFile").message;
        CHECK(!message.starts_with("Windows error") && chinese(message) != message);
    }
}
#endif

} // namespace

void plain_words_tests()
{
    item_details_are_plain_words();
    failure_classes_are_plain_words();
    one_is_said_in_the_singular();
#ifdef _WIN32
    windows_errors_are_plain_words();
#endif
}
