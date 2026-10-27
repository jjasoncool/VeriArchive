// Traditional Chinese catalog (decision 0017). Each entry pairs the English
// text written in the code with its translation; "{}" marks a value and must
// appear the same number of times on both sides ("{0}", "{1}" when the Chinese
// word order differs). Terms follow the table in decision 0017.
//
// An entry may add a match mode (text.h): Match::Cause for a wrapper whose value
// after ": " is an engine message ("context: {}", which a hint may follow),
// Match::FormatOnly for a template used only through Text::format. Without one,
// values are carried over as they are. Library and system messages (SQLite,
// JSON, strerror) have entries too: they arrive as the cause inside engine
// messages.
#include "app/text.h"

#include <span>

namespace va::app {
namespace {

constexpr CatalogEntry entries[] = {
    // Results and states shown to people; --json keeps the English codes.
    {"PASSED", "通過"},
    {"PASSED_WITH_WARNINGS", "通過（有警告）"},
    {"FAILED", "失敗"},
    {"INCOMPLETE", "未完成"},
    {"OVERDUE", "逾期"},
    {"REGISTERED", "已註冊"},
    {"RETIRED", "已停用"},
    // Comparison results (CLI and GUI); the result line keeps the mode word:
    // "比較結果：不同", as verification's "完整驗證 通過".
    {"IDENTICAL", "相同"},
    {"IDENTICAL_WITH_WARNINGS", "相同（有警告）"},
    {"DIFFERENT", "不同"},

    // Item statuses (HANDOFF §A6).
    {"Verified", "已驗證"},
    {"Modified", "已修改"},
    {"Missing", "遺失"},
    {"New", "新增"},
    {"MetadataMismatch", "中繼資料不符"},
    {"Unrepresentable", "無法表示的名稱"},
    {"TypeChanged", "類型改變"},
    {"SkippedMountPoint", "略過掛載點"},
    {"Unbaselined", "無基準"},
    {"Unreadable({})", "無法讀取（{}）"},
    {"permission", "權限不足"},
    {"media", "媒體錯誤"},
    {"other", "其他錯誤"},

    // Run-level failure classes.
    {"ManifestHashMismatch", "清單檔雜湊不符"},
    {"ManifestRevisionRollback", "清單檔版次倒退"},
    {"MediaIdentityMismatch", "媒體身分不符"},
    {"FilesystemAccessError", "檔案系統存取錯誤"},
    {"RepositoryError", "儲存庫錯誤"},

    // Media identity results.
    {"MATCH", "相符"},
    {"MATCH_WEAK", "弱相符"},
    {"MIGRATED", "已遷移"},
    {"MISMATCH", "不符"},
    {"UNKNOWN", "未知"},
    {"AMBIGUOUS", "無法確定"},
    {"PREVIOUS_CARRIER", "先前的載體"},

    // Command line
    // src/cli/cli.cpp
    {"Usage: veriarchive-cli [--config P] [--repository P] [--lang en|zh-TW] [--json] <command>\n"
        "\n"
        "  repo init | info | rebuild-index | verify-history\n"
        "  media list\n"
        "  register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision]\n"
        "           [--force-new-archive] [--copy-of UUID] [--write-media-copy]\n"
        "           [--resume [--confirm]] [--lost-manifest UUID [--confirm]]\n"
        "  verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd]\n"
        "         [--resume RUN_ID]\n"
        "  archive list | show <uuid> | retire <uuid> [--confirm]\n"
        "  archive relocate <uuid> <root_path> [--resume]\n"
        "  archive compare <uuid> <uuid>\n"
        "  archive abandon-registration <uuid> [--confirm] | abandon-relocation <uuid> [--confirm]\n"
        "  history <uuid>\n"
        "  export-sha256sums <uuid> --out FILE\n"
        "  export <uuid> --out DIR | import <file.vam> [--force-rollback]\n",
        "用法：veriarchive-cli [--config P] [--repository P] [--lang en|zh-TW] [--json] <command>\n"
        "\n"
        "  repo init | info | rebuild-index | verify-history\n"
        "  media list\n"
        "  register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision]\n"
        "           [--force-new-archive] [--copy-of UUID] [--write-media-copy]\n"
        "           [--resume [--confirm]] [--lost-manifest UUID [--confirm]]\n"
        "  verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd]\n"
        "         [--resume RUN_ID]\n"
        "  archive list | show <uuid> | retire <uuid> [--confirm]\n"
        "  archive relocate <uuid> <root_path> [--resume]\n"
        "  archive compare <uuid> <uuid>\n"
        "  archive abandon-registration <uuid> [--confirm] | abandon-relocation <uuid> [--confirm]\n"
        "  history <uuid>\n"
        "  export-sha256sums <uuid> --out FILE\n"
        "  export <uuid> --out DIR | import <file.vam> [--force-rollback]\n"},
    // src/cli/cli_support.cpp
    {"error: {}", "錯誤：{}", Match::Cause},
    {"warning: {}", "警告：{}", Match::Cause},
    {"option {} needs a value", "選項 {} 需要一個值"},
    {"--lang takes en or zh-TW", "--lang 只接受 en 或 zh-TW"},
    {"unknown option {}", "未知的選項 {}"},
    {"option {} is not used by this command", "此指令不使用選項 {}"},
    {"scanning: {} entries", "掃描中：{} 個項目"},
    {"hashing: {}", "計算雜湊中：{}", Match::FormatOnly},
    {"comparing: {}/{} entries", "比較中：{}/{} 個項目"},
    // src/cli/repo_commands.cpp
    {"RepositoryRoot: {}", "儲存庫根目錄：{}"},
    {"repository_uuid: {}", "repository_uuid：{}"},
    {"repo_selected_by: {}", "儲存庫選定方式：{}"},
    {"config_file: {}", "設定檔：{}"},
    {"Back up RepositoryRoot: losing it loses all verification history.",
        "請備份儲存庫根目錄：遺失它會失去所有驗證歷程。"},
    {"explicit", "明確指定"},
    {"pinned", "固定於設定檔"},
    {"first-run:exe-dir", "第一次使用（程式所在資料夾）"},
    {"first-run:home", "第一次使用（個人資料夾）"},
    {"History intact: {} verification runs, hash chain verified.",
        "歷程完整：共 {} 次驗證執行，雜湊鏈已驗證。"},
    // What a check of the chain tells, from src/app/history.h; the GUI shows it too.
    // Signed history checkpoints are 歷程簽章, kept apart from the 檢查點 of a resume.
    {"Checking the chain detects a recorded run that was changed or removed, but not the removal of the newest "
        "runs, nor runs changed with the chain computed again; history signatures, which would detect those, are "
        "not part of this version.",
        "檢查歷程鏈可偵測已記錄的執行紀錄被修改或刪除，但偵測不到最新幾筆執行紀錄被刪除，也偵測不到修改後連雜湊鏈都"
        "重新計算過的紀錄；能偵測這些情況的歷程簽章，這個版本尚未提供。"},
    {"History BROKEN at run {}: {}", "歷程於執行 {} 處斷裂：{}"},
    {"previous row was deleted or edited (prev_hash does not link)",
        "前一筆紀錄已被刪除或修改（prev_hash 無法銜接）"},
    {"row was edited (row_hash does not match its contents)",
        "此筆紀錄已被修改（row_hash 與其內容不符）"},
    {"Rebuilt the index from manifests: {} archives, {} revisions, {} entries.",
        "已從清單檔重建索引：{} 個封存、{} 個版次、{} 個項目。"},
    {"unfinished registrations whose saved progress was kept: {}", "保留了已儲存進度的未完成註冊：{} 個"},
    // Each revision whose manifest the rebuild could not read, by what became
    // of its record (decision 0028): the archive, then what is wrong.
    {"kept as recorded: {} ({}): {}", "保留原紀錄：{}（{}）：{}", Match::Cause},
    {"left out of the index: {} ({}): {}", "已從索引移除：{}（{}）：{}", Match::Cause},
    {"not in the index: {} ({}): {}", "不在索引中：{}（{}）：{}", Match::Cause},
    {"skipped: {}", "已略過：{}"},
    {"usage: repo init | info | rebuild-index | verify-history",
        "用法：repo init | info | rebuild-index | verify-history"},
    // src/cli/media_commands.cpp
    {"read-only", "唯讀"},
    {"filesystem_uuid: {}  short: {}", "filesystem_uuid：{}  短格式：{}"},
    {"device: {}  serial: {}  bus: {}", "裝置：{}  序號：{}  匯流排：{}"},
    {"(none)", "（無）"},
    {"usage: media list", "用法：media list"},
    // src/cli/register_commands.cpp
    {"Registered {} ({}) revision {}", "已註冊 {}（{}）版次 {}"},
    {"root: {}", "根目錄：{}"},
    {"files: {}  empty directories: {}  links: {}  hashed bytes: {}",
        "檔案：{}  空資料夾：{}  連結：{}  已計算雜湊位元組：{}"},
    {"manifest: {}", "清單檔：{}", Match::Cause},
    {"unreadable at registration ({}; recorded without hashes, never reported as verified):",
        "註冊時無法讀取（{} 個；已記錄但不含雜湊，絕不會回報為已驗證）："},
    {"skipped mount point: {}", "已略過掛載點：{}"},
    {"note: the baseline exists only in the repository; "
        "back up RepositoryRoot (or register with --write-media-copy to keep a copy on the "
        "media).",
        "注意：基準只存在於儲存庫中；請備份儲存庫根目錄（或以 --write-media-copy 註冊，"
        "在媒體上保留一份副本）。"},
    {"media copy: {}", "媒體副本：{}"},
    {"usage: register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision] "
        "[--force-new-archive] [--copy-of UUID] [--write-media-copy] [--resume [--confirm]] [--lost-manifest UUID [--confirm]]",
        "用法：register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision] "
        "[--force-new-archive] [--copy-of UUID] [--write-media-copy] [--resume [--confirm]] [--lost-manifest UUID [--confirm]]"},
    // A registration over a lost manifest (decision 0028, point 8): what it
    // would register, said before the operator confirms it, and its outcome.
    {"Lost manifest of {} ({}), revision {}", "無法取回的清單檔：{}（{}），版次 {}", Match::FormatOnly},
    {"Registering revision {} from this root takes what it holds now as the archive's new baseline, without "
        "comparing it with revision {}, whose manifest is lost: damage since revision {} was registered would not be "
        "found. Do this only where no backup of the repository, export of revision {} or media copy holds that "
        "manifest. The archive keeps its verification history and its hash chain, and never goes back to an earlier "
        "revision.",
        "從此根目錄註冊版次 {0}，會把它目前的內容當作封存新的基準，而不與清單檔已無法取回的版次 {1} 比對："
        "自版次 {2} 註冊以來發生的損壞將不會被發現。只有在儲存庫的備份、版次 {3} 的匯出檔和媒體副本中都沒有"
        "該清單檔時，才這麼做。封存會保留它的驗證歷程及其雜湊鏈，也絕不會回到較早的版次。",
        Match::FormatOnly},
    {"run the command again with --confirm to register revision {}", "若要註冊版次 {}，請加上 --confirm 再執行一次"},
    {"revision {}, whose manifest was lost, was left out of the index; its verifications stay in the history",
        "清單檔已無法取回的版次 {} 已從索引移除；它的驗證紀錄仍保留在歷程中", Match::FormatOnly},
    // A resume of a registration that an older version left, before it is
    // confirmed (decision 0022, point 7): "Unfinished registration of ...",
    // "root: {}" and "started: {}" are abandon-registration's, and the warning
    // is the Archive List's question's.
    {"run the command again with --confirm to resume it", "若要繼續，請加上 --confirm 再執行一次"},
    {"--io takes hdd or ssd", "--io 只接受 hdd 或 ssd"},
    {"not a directory: {}", "不是資料夾：{}"},
    // A root refused before anything of it is read (decision 0029), from src/app/typed_path.cpp; the
    // command line and the GUI show it.
    {"the archive root is on a network share; VeriArchive registers and verifies folders on volumes attached "
        "to this computer",
        "封存根目錄位於網路共用資料夾上；VeriArchive 只註冊和驗證這台電腦所連接的磁碟區上的資料夾"},
    {"write the archive root without {} in front (for example D:\\Photos)",
        "封存根目錄前面請不要加上 {}（例如寫成 D:\\Photos）"},
    {"write a volume's GUID path exactly as mountvol lists it, with \\\\?\\ in front",
        "磁碟區的 GUID 路徑請完全照 mountvol 列出的樣子寫，前面加上 \\\\?\\"},
    // src/cli/verify_commands.cpp
    {"FULL VERIFY {}  {} ({}) revision {}", "完整驗證 {}  {}（{}）版次 {}"},
    {"identity: {}", "媒體身分：{}", Match::Cause},
    {"coverage: {} of {} registered bytes read and matched",
        "涵蓋範圍：已註冊的 {1} 位元組中，有 {0} 位元組已讀取且相符"},
    {"items:", "項目："},
    // A run that failed on its manifest before the media was looked for (decision 0028).
    {"media: not read, as the manifest of this revision cannot be used", "媒體：未讀取，因為這個版次的清單檔無法使用"},
    {"failure: {}", "失敗：{}"},
    {"resume with: veriarchive-cli verify --full {} --resume {}",
        "若要繼續，請執行：veriarchive-cli verify --full {} --resume {}"},
    {"resumed from run: {}", "已從執行 {} 繼續"},
    {"run: {}", "執行：{}"},
    {"VeriArchive detects accidental corruption and unintended change; "
        "tamper evidence (signed manifests) is not part of this version.",
        "VeriArchive 能偵測意外損毀與非預期的變更；這個版本尚未提供竄改證據（簽章清單檔）。"},
    {"QUICK and DEEP verification are not available in this version; use --full",
        "這個版本尚未提供快速驗證與深度驗證；請使用 --full"},
    {"usage: verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd] "
        "[--resume RUN_ID]",
        "用法：verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd] "
        "[--resume RUN_ID]"},
    // src/cli/archive_commands.cpp
    {"Last verify: never", "上次驗證：從未執行"},
    {"Last FULL verify: never", "上次完整驗證：從未執行"},
    {"Last verify: {} {} on {}", "上次驗證：{} {}，於 {}"},
    {"Last FULL verify: {} on {}", "上次完整驗證：{}，於 {}"},
    {"FULL", "完整驗證"},
    {"QUICK", "快速驗證"},
    {"DEEP", "深度驗證"},
    {"REGISTRATION_INCOMPLETE", "註冊未完成"},
    {"No archives registered.", "尚未註冊任何封存。"},
    {"status: {}  revision: {}", "狀態：{}  版次：{}"},
    {"registered: {}", "註冊時間：{}"},
    {"root_subpath: {}", "根目錄子路徑：{}"},
    {"(volume root)", "（磁碟區根目錄）"},
    {"filesystem: {} {}  label \"{}\"", "檔案系統：{} {}  標籤「{}」"},
    {"device: {}  wwn {}", "裝置：{}  wwn {}"},
    {"device: {}  serial {}", "裝置：{}  序號 {}"},
    {"content: {} files, {} empty directories, {} links, {} bytes",
        "內容：{} 個檔案、{} 個空資料夾、{} 個連結、{} 位元組"},
    {"copy of: {}", "副本來源：{}"},
    {"revisions:", "版次："},
    // An archive whose latest manifest cannot be used (decision 0028), and a
    // retired archive whose latest manifest is lost (point 9), in the
    // glossary's term (a lost manifest: 無法取回的清單檔).
    {"problem: {}", "問題：{}", Match::Cause},
    {"manifest lost: {}", "清單檔無法取回：{}", Match::Cause},
    // The archive's unfinished registration and relocation in archive list and show (decision 0026), from
    // src/app/unfinished_operations.cpp; the progress and state are the Archive List's words.
    {"Unfinished registration of revision {} from \"{}\": {}; {}", "未完成的註冊（版次 {}，根目錄「{}」）：{}；{}",
        Match::FormatOnly},
    {"Unfinished relocation of revision {} to \"{}\": {}; {}", "未完成的搬移（版次 {}，至「{}」）：{}；{}",
        Match::FormatOnly},
    {"Retired {} ({}). It is no longer expected to be verified, "
        "is still recognised when attached, and keeps its history.",
        "已停用 {}（{}）。此封存不再需要定期驗證，接上時仍會被辨識，並保留其歷程。"},
    // Retiring an archive whose latest manifest is lost (decision 0028, point
    // 9): what it means, said before the operator confirms it, and its outcome.
    {"Retiring the archive declares that manifest lost for good: do this only where no backup of the repository, "
        "export of revision {} or media copy holds it, and the device that held the archive is gone as well (broken, "
        "lost or wiped); otherwise put the manifest back with `import`, or register the archive's next revision from "
        "its folder or volume with `register <root_path> --lost-manifest {}`. The archive keeps its verification "
        "history and its hash chain, but can no longer be verified or take a new revision. It no longer stops the "
        "registration of a new folder or volume or a relocation: retired, it is no longer taken as a possible owner of "
        "one, as identity matching cannot use its latest manifest.",
        "停用此封存，就是宣告該清單檔再也無法取回：只有在儲存庫的備份、版次 {0} 的匯出檔和媒體副本中都沒有該清單檔，"
        "而且存放此封存的裝置也已不在（損壞、遺失或已清除）時，才這麼做；否則請以 `import` 放回該清單檔，或以 "
        "`register <root_path> --lost-manifest {1}` 用封存的資料夾或磁碟區目前的內容註冊它的下一個版次。封存會保留"
        "它的驗證歷程及其雜湊鏈，但再也無法驗證，也不再接受新的版次。它不再讓註冊新的資料夾或磁碟區、或搬移停下來："
        "停用之後，因為比對媒體身分時無法使用它最新的清單檔，它不再被視為可能擁有它們的封存。",
        Match::FormatOnly},
    {"run the command again with --confirm to retire the archive", "若要停用此封存，請加上 --confirm 再執行一次"},
    {"Retired {} ({}), whose manifest of revision {} is lost. It keeps its verification history and its hash chain; "
        "it can no longer be verified or take a new revision, and no longer stops the registration of a new folder or "
        "volume or a relocation.",
        "已停用 {0}（{1}），它版次 {2} 的清單檔已無法取回。此封存會保留它的驗證歷程及其雜湊鏈；它再也無法驗證，也不再"
        "接受新的版次，並且不再讓註冊新的資料夾或磁碟區、或搬移停下來。",
        Match::FormatOnly},
    {"archive {} now has revision {}, not revision {}; nothing was changed (run the command again)",
        "封存 {0} 現在的版次是 {1}，而不是版次 {2}；未做任何變更（請再執行一次此命令）"},
    {"RELOCATION REFUSED: {}", "拒絕搬移：{}"},
    {"Relocated {} ({}) to {} as revision {}", "已將 {}（{}）搬移至 {}，成為版次 {}"},
    {"FULL VERIFY {} of the new carrier (run {})", "新載體的完整驗證：{}（執行 {}）"},
    {"never verified", "從未驗證"},
    {"{}  {} VERIFY {}  r{}  {} bytes", "{}  {} {}  r{}  {} 位元組", Match::FormatOnly},
    // The runs of a relocation in history and the Verification History (decision 0026), from
    // src/app/history.cpp.
    {"interrupted relocation attempt", "中斷的搬移嘗試"},
    {"relocation from r{}", "從 r{} 搬移", Match::FormatOnly},
    {"several archives start with {}", "有多個封存以 {} 開頭"},
    {"no archive {}", "找不到封存 {}"},
    {"usage: archive list | show <uuid> | retire <uuid> [--confirm] | relocate <uuid> <root_path> "
        "[--io hdd|ssd] [--resume] | compare <uuid> <uuid> | abandon-registration <uuid> [--confirm] | "
        "abandon-relocation <uuid> [--confirm]",
        "用法：archive list | show <uuid> | retire <uuid> [--confirm] | relocate <uuid> <root_path> "
        "[--io hdd|ssd] [--resume] | compare <uuid> <uuid> | abandon-registration <uuid> [--confirm] | "
        "abandon-relocation <uuid> [--confirm]"},
    // archive compare: A is the reference archive, B the copy compared with it.
    {"COMPARE {}  {} ({}) r{}  vs  {} ({}) r{}", "比較結果：{}  {}（{}）r{}  與  {}（{}）r{}"},
    {"A: {}, registered {}, {} entries", "A：{}，註冊於 {}，共 {} 個項目"},
    {"B: {}, registered {}, {} entries", "B：{}，註冊於 {}，共 {} 個項目"},
    {"same: {}", "相同：{}"},
    {"This compares what each registration read. A FULL verify of each archive proves that its "
        "carrier still holds that content.",
        "此比較依據各封存註冊時讀取的內容。對各封存進行完整驗證，才能證明其載體仍保有這些內容。"},
    {"comparing: reading the manifests", "比較中：正在讀取清單檔"},
    // archive abandon-registration (decision 0025)
    {"Unfinished registration of {} ({}), revision {}", "{}（{}）未完成的註冊，版次 {}"},
    {"started: {}", "開始時間：{}"},
    {"saved progress: {} files, {} bytes", "已儲存的進度：{} 個檔案，{} 位元組"},
    {"Abandoning it removes its saved progress, and it can never be resumed afterwards. "
        "Recorded revisions, manifests and the verification history stay as they are.",
        "放棄後，已儲存的進度會被刪除，而且之後再也無法繼續。已記錄的版次、清單檔與驗證歷程都維持不變。"},
    {"The archive has no revision yet: it is removed with its registration.",
        "此封存還沒有任何版次：會隨著它的註冊一起移除。"},
    {"run the command again with --confirm to abandon it", "若要放棄，請加上 --confirm 再執行一次"},
    {"Abandoned the unfinished registration of {} ({}), revision {}: its saved progress "
        "({} files, {} bytes) was removed.",
        "已放棄 {}（{}）未完成的註冊，版次 {}：已刪除其已儲存的進度（{} 個檔案，{} 位元組）。"},
    {"The archive had no revision: it is no longer listed.", "此封存沒有任何版次：已不再列出。"},
    {"note: the media may still hold .veriarchive/identity.json naming this archive; delete it before "
        "registering the folder or volume again, unless you register it with --write-media-copy.",
        "注意：媒體上可能仍有指向此封存的 .veriarchive/identity.json；再次註冊此資料夾或磁碟區前請先刪除它，"
        "除非註冊時加上 --write-media-copy。"},
    // archive abandon-relocation (decision 0026); "started: {}", "saved
    // progress: ..." and the --confirm line are abandon-registration's.
    {"Unfinished relocation of {} ({}), revision {}", "{}（{}）未完成的搬移，版次 {}"},
    {"new root: {}", "新根目錄：{}"},
    {"Abandoning it removes its saved progress, and it can never be resumed afterwards. The archive, its "
        "revisions, manifests and the verification history stay as they are.",
        "放棄後，已儲存的進度會被刪除，而且之後再也無法繼續。封存本身、已記錄的版次、清單檔與驗證歷程都維持不變。"},
    {"Abandoned the unfinished relocation of {} ({}): its saved progress ({} files, {} bytes) was removed.",
        "已放棄 {}（{}）未完成的搬移：已刪除其已儲存的進度（{} 個檔案，{} 位元組）。"},
    {"usage: history <uuid>", "用法：history <uuid>"},
    // src/cli/export_commands.cpp
    {"usage: export-sha256sums <uuid> --out FILE", "用法：export-sha256sums <uuid> --out FILE"},
    {"usage: export <uuid> --out DIR", "用法：export <uuid> --out DIR"},
    {"usage: import <file.vam> [--force-rollback]", "用法：import <file.vam> [--force-rollback]"},
    {"Wrote {} for {} revision {}. Check it from the archive root with: sha256sum -c <file>",
        "已寫入 {}，對應 {} 版次 {}。請在封存根目錄執行以下指令檢查：sha256sum -c <file>"},
    {"{}: no hash (unreadable at registration)", "{}：沒有雜湊（註冊時無法讀取）"},
    {"{}: name is not valid Unicode ({})", "{}：名稱不是有效的 Unicode（{}）"},
    {"Exported {}\n"
        "     and {}",
        "已匯出 {}\n"
        "    及 {}"},
    {"Already present: {} ({}) revision {}, identical bytes.",
        "已存在：{}（{}）版次 {}，位元組完全相同。"},
    {"Imported {} ({}) revision {} as a new archive.",
        "已匯入 {}（{}）版次 {}，並建立為新的封存。"},
    {"Imported {} ({}) revision {}.", "已匯入 {}（{}）版次 {}。"},
    // A recorded revision's manifest put back by an import (decision 0028).
    {"Restored the manifest of {} ({}) revision {}: the file is the one the repository recorded.",
        "已還原 {}（{}）版次 {} 的清單檔：此檔案就是儲存庫所記錄的那一份。"},
    // src/app/progress_estimate.cpp: the progress of hashing, after "hashing: "
    // (CLI) or "Hashing: " (GUI), e.g.
    // "計算雜湊中：2.16 TiB / 9.44 TiB（23%）・每秒 240 MiB・約剩 8～9 小時".
    {"{} of {} ({}%)", "{} / {}（{}%）", Match::FormatOnly},
    {"{} of {} files ({}%)", "{} / {} 個檔案（{}%）", Match::FormatOnly},
    {"{}/s", "每秒 {}", Match::FormatOnly},
    {"{} · {}", "{}・{}", Match::FormatOnly},
    {"estimating time left", "剩餘時間估算中"},
    {"less than a minute left", "剩不到 1 分鐘"},
    {"less than {} minutes left", "剩不到 {} 分鐘", Match::FormatOnly},
    {"about 1 minute left", "約剩 1 分鐘"},
    {"about {} minutes left", "約剩 {} 分鐘", Match::FormatOnly},
    {"about {}-{} minutes left", "約剩 {}～{} 分鐘", Match::FormatOnly},
    {"about 1 hour left", "約剩 1 小時"},
    {"about {} hours left", "約剩 {} 小時", Match::FormatOnly},
    {"about {}-{} hours left", "約剩 {}～{} 小時", Match::FormatOnly},
    {"about {} days left", "約剩 {} 天", Match::FormatOnly},
    {"about {}-{} days left", "約剩 {}～{} 天", Match::FormatOnly},

    // Graphical interface
    // src/gui/gui_app.cpp
    {"Archive List", "封存列表"},
    {"Register Archive", "註冊封存"},
    {"Verify Archive", "驗證封存"},
    {"Compare Archives", "比較封存"},
    {"Verification History", "驗證歷程"},
    {"Settings", "設定"},
    // src/gui/gui_support.cpp
    {"Scanning: {} entries", "掃描中：{} 個項目"},
    {"Hashing: {}", "計算雜湊中：{}", Match::FormatOnly},
    {"Comparing: {} / {} entries", "比較中：{} / {} 個項目"},
    {"Starting...", "正在啟動……"},
    {"Unexpected error: {}", "未預期的錯誤：{}"},
    // src/gui/archive_list_view.cpp
    {"never", "從未"},
    {"Problem: {}", "問題：{}", Match::FormatOnly},  // an archive whose manifest cannot be used (decision 0028)
    // A retired archive whose latest manifest is lost (decision 0028, point 9).
    {"Manifest lost: {}", "清單檔無法取回：{}", Match::FormatOnly},
    {"No repository is open; see Settings.", "目前沒有開啟的儲存庫；請參閱「設定」。"},
    {"{} archive", "{} 個封存", Match::FormatOnly},
    {"{} archives", "{} 個封存", Match::FormatOnly},
    {"Label", "標籤"},
    {"UUID", "UUID"},
    {"Volume", "磁碟區"},
    {"Model", "型號"},
    {"Capacity", "容量"},
    {"Last FULL verify", "上次完整驗證"},
    {"Last verify", "上次驗證"},
    {"Overdue", "逾期"},
    {"Rev", "版次"},
    {"Status", "狀態"},
    {"Archives", "封存列表"},
    {"Each archive shows when it was last read in full (FULL verify) and when it was last "
        "verified at all. A result is only as current as its date.",
        "每個封存都會顯示上次完整讀取（完整驗證）的時間，以及上次進行任何驗證的時間。"
        "結果只反映其日期當時的狀態。"},
    {"Refresh", "重新整理"},
    // src/gui/archive_list_view.cpp, src/app/unfinished_operations.cpp: unfinished
    // operations (decision 0022)
    {"Unfinished operations", "未完成的作業"},
    {"Registrations, verifications and relocations that have not finished, also those started from the "
        "command line. The numbers move each time an operation saves its progress: after every 1000 files, "
        "or once a minute when a file finishes; not while it scans.",
        "尚未完成的註冊、驗證與搬移，包括從命令列啟動的。每當作業儲存進度，數字就會更新：每 1000 個檔案一次，"
        "或在檔案完成時每分鐘一次；掃描期間不會更新。"},
    {"Root", "根目錄"},
    {"Operation", "作業"},
    {"Progress", "進度"},
    {"Registration", "註冊"},
    {"Verification", "驗證"},
    {"Relocation", "搬移"},
    {"{} of about {} ({}%)", "{} / 約 {}（{}%）", Match::FormatOnly},
    {"{} done", "已完成 {}", Match::FormatOnly},
    {"total not known: {}", "總量不明：{}", Match::FormatOnly},
    {"{} of {} files", "{} / {} 個檔案", Match::FormatOnly},
    {"{} files", "{} 個檔案", Match::FormatOnly},
    {"waiting for progress...", "等待進度更新……"},
    {"running", "執行中"},
    {"no progress seen for {} minutes; it may have stopped", "已 {} 分鐘未見進度，可能已停止", Match::FormatOnly},
    {"no progress seen for {} hours; it may have stopped", "已 {} 小時未見進度，可能已停止", Match::FormatOnly},
    {"stopped; it can be resumed", "已停止；可以繼續"},
    {"stopped; it cannot be resumed (the archive has a newer revision)", "已停止；無法繼續（封存已有較新的版次）"},
    // A relocation whose new carrier does not hold the recorded content (decision 0026).
    {"new carrier differs; abandon it", "新載體上的內容不符；請放棄它"},
    // A registration or relocation made for an older revision (decision 0030).
    {"archive has a later revision; abandon it", "封存已有較新的版次；請放棄它"},
    // A registration over the lost manifest of an archive retired since
    // (decision 0028, point 9).
    {"archive is retired; abandon it", "封存已停用；請放棄它"},
    {"not counted", "未計算"},
    {"not known", "不明"},
    {"no progress reported for {} minutes; its process is still running", "已 {} 分鐘未回報進度；其處理程序仍在執行",
        Match::FormatOnly},
    {"no progress reported for {} hours; its process is still running", "已 {} 小時未回報進度；其處理程序仍在執行",
        Match::FormatOnly},
    {"Process: {} ({})", "處理程序：{}（{}）", Match::FormatOnly},
    // Resuming a stopped verification or registration from the list (decision 0022, point 7).
    {"Resume", "繼續"},
    {"This verification is no longer listed as unfinished.", "此驗證已不在未完成的作業中。"},
    // Resume asks first on a registration that an older version left and that
    // may have stopped (decision 0022, point 7; operator, 2026-10-20); the
    // warning is src/app/unfinished_operations.h's, and "Root: {}", "Cancel"
    // and "Resume" are the views' own.
    {"Resume the unfinished registration of {}?", "要繼續 {} 未完成的註冊嗎？"},
    {"An older version of VeriArchive left this registration unfinished. That version does not record when it "
        "last worked, so VeriArchive cannot tell whether it has stopped or is still reading a large file or "
        "scanning the folder or volume. If it is still running, it will later record its own manifest over the one "
        "that this resume records: from then on every verification of the archive fails, and other registrations "
        "and relocations stop until the archive gets a new revision. Make sure that the older version's window or "
        "command has been closed before you resume.",
        "舊版 VeriArchive 留下了這個未完成的註冊。舊版不會記錄它最後一次運作的時間，所以程式無法判斷它已經停止，"
        "還是仍在讀取一個大檔案、或掃描資料夾或磁碟區。如果它仍在執行，之後會用自己的清單檔蓋掉這次繼續所記錄的"
        "清單檔：從此這個封存的每次驗證都會失敗，其他的註冊與搬移也都會停下來，直到這個封存有了新的版次為止。"
        "繼續之前，請先確認舊版的視窗或命令都已關閉。"},
    {"Abandon...", "放棄…"},
    {"Abandon", "放棄"},
    {"Abandon the unfinished registration of {}?", "要放棄 {} 未完成的註冊嗎？"},
    {"Its saved progress will be lost: {} files ({}) hashed so far.",
        "已儲存的進度會遺失：目前已計算雜湊的 {} 個檔案（{}）。"},
    {"It cannot be resumed afterwards. Recorded revisions, manifests and the verification history are not changed.",
        "之後再也無法繼續。已記錄的版次、清單檔與驗證歷程都不會變更。"},
    {"Reading the registration of {} ...", "正在讀取 {} 的註冊……"},
    {"Abandoning the registration of {} ...", "正在放棄 {} 的註冊……"},
    {"Abandoned the unfinished registration of {} ({}), revision {}.", "已放棄 {}（{}）未完成的註冊，版次 {}。"},
    {"Note: the media may still hold .veriarchive/identity.json naming this archive; delete it before "
        "registering the folder or volume again, unless you register it with a media copy.",
        "注意：媒體上可能仍有指向此封存的 .veriarchive/identity.json；再次註冊此資料夾或磁碟區前請先刪除它，"
        "除非註冊時同時寫入媒體副本。"},
    {"Not abandoned: {}", "未能放棄：{}"},
    {"This registration is no longer listed as unfinished.", "此註冊已不在未完成的作業中。"},
    // Abandoning a relocation (decision 0026); "Abandon", "Cancel" and "Not
    // abandoned: {}" are the registration's.
    {"Abandon the unfinished relocation of {}?", "要放棄 {} 未完成的搬移嗎？"},
    {"New root: {}", "新根目錄：{}"},
    {"Its saved progress will be lost: {} files ({}) matched so far.",
        "已儲存的進度會遺失：目前已比對相符的 {} 個檔案（{}）。"},
    {"It cannot be resumed afterwards. The archive, its revisions, manifests and the verification history are "
        "not changed.",
        "之後再也無法繼續。封存本身、已記錄的版次、清單檔與驗證歷程都不會變更。"},
    {"Reading the relocation of {} ...", "正在讀取 {} 的搬移……"},
    {"Abandoning the relocation of {} ...", "正在放棄 {} 的搬移……"},
    {"Abandoned the unfinished relocation of {} ({}).", "已放棄 {}（{}）未完成的搬移。"},
    {"This relocation is no longer listed as unfinished.", "此搬移已不在未完成的作業中。"},
    {"Cannot read the unfinished operations: {}", "無法讀取未完成的作業：{}", Match::FormatOnly},
    // src/gui/history_view.cpp
    {"Finished", "完成時間"},
    {"Mode", "模式"},
    {"Result", "結果"},
    {"Revision", "版次"},
    {"Bytes matched", "相符位元組"},
    {"Failures", "失敗類別"},
    {"Run", "執行"},
    {"Note", "備註"},
    {"Never verified.", "從未驗證。"},
    {"Cannot check the history: {}", "無法檢查歷程：{}"},
    {"Archive:", "封存："},
    {"Show runs", "顯示執行紀錄"},
    {"Check history chain", "檢查歷程鏈"},
    // src/gui/settings_view.cpp
    {"No repository is open.", "目前沒有開啟的儲存庫。"},
    {"On first use, initialise a repository: it is created next to the program (or in your "
        "home folder when that is not writable) and remembered in the config file.",
        "第一次使用時，請初始化儲存庫：它會建立在程式所在資料夾（若該處無法寫入，"
        "則建立在您的個人資料夾），並記錄在設定檔中。"},
    {"Selected by: {}", "選定方式：{}"},
    {"Config file: {}", "設定檔：{}"},
    {"Overdue after: FULL verify {} days, any verify {} days",
        "逾期期限：完整驗證 {} 天，任何驗證 {} 天"},
    {"Strict metadata: on", "嚴格中繼資料：開啟"},
    {"Strict metadata: off", "嚴格中繼資料：關閉"},
    {"Trusted signing keys: signed manifests are not part of this version.",
        "受信任的金鑰：這個版本尚未提供簽章清單檔。"},
    {"Back up RepositoryRoot. Losing it loses all verification history; "
        "manifests can be recovered only from media copies, if they were written.",
        "請備份儲存庫根目錄。遺失它會失去所有驗證歷程；"
        "清單檔只能從媒體副本復原（若曾寫入媒體副本）。"},
    {"Warning: {}", "警告：{}"},
    {"Initialise repository", "初始化儲存庫"},
    {"repository_uuid changed from {} to {}; recorded the new uuid",
        "repository_uuid 已從 {} 變更為 {}；已記錄新的 uuid"},
    {"VeriArchive is running elevated; repository and home resolution may differ",
        "VeriArchive 正以系統管理員權限執行；儲存庫與個人資料夾的判定結果可能不同"},
    {"cannot locate the config file", "找不到設定檔的位置"},
    {"Finish or cancel the running operation before changing the language.",
        "請先完成或取消正在執行的作業，再切換語言。"},
    // src/gui/register_view.cpp
    {"{} files, {} empty directories, {} links, {} hashed",
        "{} 個檔案、{} 個空資料夾、{} 個連結，已計算雜湊 {}",
        Match::FormatOnly},
    {"Manifest: {}", "清單檔：{}"},
    {"Unreadable at registration (recorded without hashes, never reported as verified):",
        "註冊時無法讀取（已記錄但不含雜湊，絕不會回報為已驗證）："},
    {"Skipped mount point: {}", "已略過掛載點：{}"},
    {"The baseline exists only in the repository: back up RepositoryRoot.",
        "基準只存在於儲存庫中：請備份儲存庫根目錄。"},
    {"Media copy: {}", "媒體副本：{}"},
    // A registration over a lost manifest resumed in Register Archive (decision 0028, point 8).
    {"Revision {}, whose manifest was lost, was left out of the index; its verifications stay in the history.",
        "清單檔已無法取回的版次 {} 已從索引移除；它的驗證紀錄仍保留在歷程中。", Match::FormatOnly},
    {"Another operation is running.", "已有其他作業正在執行。"},
    {"Open a repository first (Settings).", "請先開啟儲存庫（見「設定」）。"},
    {"Choose the folder or volume to register.", "請選擇要註冊的資料夾或磁碟區。"},
    {"Registering {} ...", "正在註冊 {}……"},
    {"Cannot open the repository: {}", "無法開啟儲存庫：{}"},
    {"Not registered: {}", "未能註冊：{}"},
    {"Done", "完成"},
    {"Registration reads every file of the folder or volume and records its fingerprints "
        "(BLAKE3 and SHA-256) as the baseline that later verifications compare against. "
        "Nothing is written to the archive unless you ask for a media copy.",
        "註冊會讀取資料夾或磁碟區中的每個檔案，並記錄其指紋（BLAKE3 與 SHA-256）作為基準，"
        "供日後驗證比對。除非您要求寫入媒體副本，否則不會對封存寫入任何內容。"},
    {"Archive folder or volume root", "封存資料夾或磁碟區根目錄"},
    {"Choose...", "選擇…"},
    {"Choose the archive folder or volume", "選擇封存資料夾或磁碟區"},
    {"Label (optional, e.g. ARCH-0012)", "標籤（選填，例如 ARCH-0012）"},
    {"New revision of the archive registered at this folder or volume",
        "為此資料夾或磁碟區已註冊的封存建立新版次"},
    {"Register as a separate archive even though this folder or volume is registered",
        "即使此資料夾或磁碟區已註冊，仍註冊為另一個封存"},
    {"Also write a manifest copy to the media (.veriarchive)",
        "同時將清單檔寫入媒體作為媒體副本（.veriarchive）"},
    {"Resume an interrupted registration of this folder or volume", "繼續此資料夾或磁碟區中斷的註冊"},
    {"Register", "註冊"},
    {"Cancel", "取消"},
    // src/gui/verify_view.cpp
    {"FULL VERIFY {}", "完整驗證 {}"},
    {"{} ({}) revision {}", "{}（{}）版次 {}", Match::FormatOnly},
    {"Root: {}", "根目錄：{}"},
    {"Identity: {}", "媒體身分：{}"},
    {"Coverage: {} of {} registered bytes read and matched",
        "涵蓋範圍：已註冊的 {1} 中，有 {0} 已讀取且相符"},
    {"Items:", "項目："},
    {"Media: not read, as the manifest of this revision cannot be used", "媒體：未讀取，因為這個版次的清單檔無法使用"},
    {"Failure: {}", "失敗：{}"},
    {"The run was interrupted; use Resume to continue without re-reading verified files.",
        "此次執行已中斷；使用「繼續中斷的執行」即可接續，不必重新讀取已驗證的檔案。"},
    {"Resumed from run: {}", "已從執行 {} 繼續"},
    {"Run: {}", "執行：{}"},
    {"Choose the folder or volume to verify, or pick a registered archive.",
        "請選擇要驗證的資料夾或磁碟區，或挑選已註冊的封存。"},
    {"Verifying (reading every registered byte) ...", "正在驗證（讀取每個已註冊的位元組）……"},
    {"Not verified: {}", "未能驗證：{}"},
    // A chosen archive that more than one attached volume holds (decision 0022, point 7).
    {"Not verified: archive {} is on more than one attached volume, as a device and a sector-by-sector clone of it "
        "would be:{}\nVeriArchive cannot tell which one to verify. Type the folder of the one to verify above, or "
        "choose it with \"Choose...\", choose \"The folder or volume above\" under \"Verify:\" and press \"Verify "
        "(FULL)\"; or detach the other volumes and verify the archive again.",
        "未能驗證：封存 {} 位於多個已連接的磁碟區上，例如一個裝置和它逐磁區複製出來的複本：{}\n程式無法判斷要驗證哪一個。"
        "請在上方輸入要驗證的那一個的資料夾，或按「選擇…」挑選它，在「驗證：」選擇「上方的資料夾或磁碟區」，"
        "再按「完整驗證」；或讓其他磁碟區中斷連線後，再驗證一次這個封存。",
        Match::FormatOnly},
    {"The folder or volume above", "上方的資料夾或磁碟區"},
    {"FULL verification re-reads every registered file from the medium and compares it with "
        "the baseline. Only FULL VERIFY PASSED means 100% of the registered content was read and "
        "matched.",
        "完整驗證會從媒體重新讀取每個已註冊的檔案，並與基準比對。"
        "只有「完整驗證 通過」才代表 100% 的已註冊內容都已讀取且相符。"},
    {"Verify:", "驗證："},
    {"Strict metadata (timestamp and attribute differences fail the run)",
        "嚴格中繼資料（時間戳記與屬性差異會使執行失敗）"},
    {"Verify (FULL)", "完整驗證"},
    {"Resume interrupted run", "繼續中斷的執行"},
    // src/gui/compare_view.cpp
    {"ARCHIVES {}", "比較結果：{}"},
    {"Every entry of A is in B with the same type and content, and B has no other entries.",
        "A 的每個項目都以相同的類型與內容存在於 B，且 B 沒有其他項目。"},
    {"No entry differs, but the entries listed below are not proven the same.",
        "沒有項目不同，但下列項目無法證明相同。"},
    {"A (reference): {} ({}) revision {}, registered {}", "A（參考）：{}（{}）版次 {}，註冊於 {}"},
    {"B (copy): {} ({}) revision {}, registered {}", "B（副本）：{}（{}）版次 {}，註冊於 {}"},
    {"Entries: {} in A, {} in B, {} the same", "項目：A 有 {} 個，B 有 {} 個，其中 {} 個相同"},
    {"Differences: {}", "差異：{}"},
    {"... and {} more differences, not listed here", "……另有 {} 個差異未列於此"},
    {"This compares what each registration read. FULL verification of each archive proves that "
        "its media still holds it.",
        "此比較依據各封存註冊時讀取的內容。對各封存進行完整驗證，才能證明其媒體仍保有這些內容。"},
    {"Not compared: {}", "未能比較：{}"},
    {"Choose the two archives to compare.", "請選擇要比較的兩個封存。"},
    {"A and B are the same archive; choose the copy to check as B.",
        "A 與 B 是同一個封存；請將要檢查的副本選為 B。"},
    {"Comparing the manifests of A and B ...", "正在比較 A 與 B 的清單檔……"},
    {"Comparison checks that two registered archives hold the same entries with the same "
        "content, using their manifests in the repository; no media is read. Choose the reference "
        "archive as A and the copy to check as B.",
        "比較會使用儲存庫中的清單檔，檢查兩個已註冊的封存是否有相同的項目與內容；不會讀取媒體。"
        "請將參考封存選為 A，要檢查的副本選為 B。"},
    {"Reference archive (A):", "參考封存（A）："},
    {"Copy to check (B):", "要檢查的副本（B）："},
    {"Compare", "比較"},

    // Engine and repository messages
    // src/core/engine/register_engine.cpp
    {"the root overlaps registered archive {} (volume root); "
        "archive roots on one volume may not contain each other (decision 0001)",
        "根目錄與已註冊的封存 {}（磁碟區根目錄）重疊；"
        "同一磁碟區上的封存根目錄不得互相包含（決策紀錄 0001）"},
    {"the root overlaps registered archive {} (root \"{}\"); "
        "archive roots on one volume may not contain each other (decision 0001)",
        "根目錄與已註冊的封存 {}（根目錄「{}」）重疊；"
        "同一磁碟區上的封存根目錄不得互相包含（決策紀錄 0001）"},
    {"this media is an earlier carrier of archive {} (volume root); "
        "pass --force-new-archive to register it separately (decision 0004)",
        "此媒體是封存 {}（磁碟區根目錄）先前的載體；若要另行註冊，"
        "請加上 --force-new-archive（決策紀錄 0004）"},
    {"this media is an earlier carrier of archive {} (root \"{}\"); "
        "pass --force-new-archive to register it separately (decision 0004)",
        "此媒體是封存 {}（根目錄「{}」）先前的載體；若要另行註冊，"
        "請加上 --force-new-archive（決策紀錄 0004）"},
    {"this filesystem belongs to archive {} (volume root) but the device differs (MIGRATED); "
        "use `archive relocate`, or pass --force-new-archive",
        "此檔案系統屬於封存 {}（磁碟區根目錄），但裝置不同（已遷移）；請使用 `archive relocate`，"
        "或加上 --force-new-archive"},
    {"this filesystem belongs to archive {} (root \"{}\") but the device differs (MIGRATED); "
        "use `archive relocate`, or pass --force-new-archive",
        "此檔案系統屬於封存 {}（根目錄「{}」），但裝置不同（已遷移）；請使用 `archive relocate`，"
        "或加上 --force-new-archive"},
    {"archive {} (volume root) is retired and takes no new revisions (decision 0003)",
        "封存 {}（磁碟區根目錄）已停用，不再接受新的版次（決策紀錄 0003）"},
    {"archive {} (root \"{}\") is retired and takes no new revisions (decision 0003)",
        "封存 {}（根目錄「{}」）已停用，不再接受新的版次（決策紀錄 0003）"},
    {"several archives match this root (AMBIGUOUS); resolve the clone first",
        "有多個封存符合此根目錄（無法確定）；請先處理複製的媒體"},
    {"already registered as archive {} (volume root); "
        "pass --new-revision to re-baseline it or --force-new-archive (R-97)",
        "已註冊為封存 {}（磁碟區根目錄）；請加上 --new-revision 重新建立基準，"
        "或加上 --force-new-archive（R-97）"},
    {"already registered as archive {} (root \"{}\"); "
        "pass --new-revision to re-baseline it or --force-new-archive (R-97)",
        "已註冊為封存 {}（根目錄「{}」）；請加上 --new-revision 重新建立基準，"
        "或加上 --force-new-archive（R-97）"},
    {"--new-revision: no registered archive at this root",
        "--new-revision：此根目錄沒有已註冊的封存"},
    {"labels have the form ARCH-NNNN", "標籤的格式為 ARCH-NNNN"},
    {"--copy-of needs an archive uuid", "--copy-of 需要一個封存 uuid"},
    {"cannot identify the volume of the root: {}", "無法識別根目錄所在的磁碟區：{}", Match::Cause},
    {"there is no interrupted registration of this root to resume", "此根目錄沒有可繼續的中斷註冊"},
    {"the archive of the interrupted registration is missing", "找不到中斷的註冊所屬的封存"},
    {"an interrupted registration of this root exists (archive {}); finish it with --resume, or abandon it "
        "with `archive abandon-registration {}`",
        "此根目錄有一個中斷的註冊（封存 {}）；請以 --resume 完成它，或以 `archive abandon-registration {}` 放棄它"},
    {"the archive to re-baseline is missing from the database",
        "資料庫中找不到要重新建立基準的封存"},
    {"cannot write the media copy: {}", "無法寫入媒體副本：{}", Match::Cause},
    {"registration was interrupted while scanning; "
        "continue with `register <root_path> --resume`",
        "註冊在掃描時中斷；請以 `register <root_path> --resume` 繼續"},
    {"the device disappeared during registration; "
        "continue with `register <root_path> --resume`",
        "裝置在註冊期間中斷連線；請以 `register <root_path> --resume` 繼續"},
    {"registration was cancelled; continue with `register <root_path> --resume`",
        "註冊已取消；請以 `register <root_path> --resume` 繼續"},
    {"cannot scan the root: {}", "無法掃描根目錄：{}", Match::Cause},
    {"cannot write the manifest: {}", "無法寫入清單檔：{}", Match::Cause},
    {"cannot record the archive: {}", "無法記錄封存：{}", Match::Cause},
    {"revision {} of archive {} is already recorded (by another attempt of this registration or by "
        "another operation, such as a relocation); this attempt recorded nothing",
        "封存 {1} 的版次 {0} 已有紀錄（由同一個註冊的另一次執行，或由其他作業，例如搬移）；"
        "這次執行未記錄任何內容"},
    {"this registration is still running in process {} ({}) on this computer; "
        "resume it once that process has ended",
        "此註冊仍在這台電腦的處理程序 {}（{}）中執行；請等該處理程序結束後再繼續"},
    {"the unfinished registration of archive {} was abandoned while this attempt ran; "
        "this attempt recorded nothing",
        "封存 {} 未完成的註冊已在這次執行期間被放棄；這次執行未記錄任何內容"},
    // A registration made for an older revision never records (decision 0030).
    {"the unfinished registration of archive {} (root \"{}\") would create revision {}, but the archive now has a "
        "later revision ({}); the registration can no longer finish: abandon it with `archive abandon-registration {}`",
        "封存 {} 未完成的註冊（根目錄「{}」）原本要建立版次 {}，但封存現在已有較新的版次（{}）；這個註冊再也無法完成："
        "請以 `archive abandon-registration {}` 放棄它"},
    {"revision {} of archive {} was recorded by another operation before this registration could begin; nothing was "
        "changed (run `register --new-revision` again)",
        "封存 {1} 的版次 {0} 已在這次註冊開始前由其他作業記錄；未做任何變更（請再執行一次 `register --new-revision`）"},
    // A registration over a lost manifest (decision 0028, point 8), refused.
    {"--lost-manifest takes none of --label, --new-revision, --force-new-archive and --resume",
        "--lost-manifest 不能與 --label、--new-revision、--force-new-archive 或 --resume 一起使用"},
    {"--confirm is used only with --lost-manifest or --resume", "--confirm 只能與 --lost-manifest 或 --resume 一起使用"},
    // A retired archive takes no new revision, not even over its lost manifest
    // (decision 0028, point 9): a registration of it that an older version
    // began waits to be abandoned.
    {"archive {} is retired and takes no new revisions (decision 0003): abandon its unfinished registration with "
        "`archive abandon-registration {}`",
        "封存 {} 已停用，不再接受新的版次（決策紀錄 0003）：請以 `archive abandon-registration {}` 放棄它未完成的註冊"},
    {"the manifest of revision {} of archive {} can be used: register its next revision with `register <root_path> "
        "--new-revision`",
        "封存 {1} 版次 {0} 的清單檔可以使用：請以 `register <root_path> --new-revision` 註冊它的下一個版次"},
    // "Not lost" in the glossary's term (a lost manifest: 無法取回的清單檔),
    // never 遺失, which this catalog gives a manifest that is missing.
    {"the manifest of revision {} of archive {} is not lost: {}", "封存 {1} 版次 {0} 的清單檔並非無法取回：{2}",
        Match::Cause},
    {"the identity file of this root (.veriarchive/identity.json) names archive {}, not archive {}",
        "此根目錄的身分檔案（.veriarchive/identity.json）指向封存 {}，而不是封存 {}"},
    {"the media copy at this root holds the manifest of revision {} of archive {}: put it back with `import {}`",
        "此根目錄的媒體副本中有封存 {1} 版次 {0} 的清單檔：請以 `import {2}` 放回該清單檔"},
    {"the root overlaps the root of archive {}, whose latest manifest records this volume's filesystem id and device "
        "(decision 0001)",
        "根目錄與封存 {} 的根目錄重疊，而該封存最新的清單檔所記錄的正是此磁碟區的檔案系統 UUID 與裝置（決策紀錄 0001）"},
    // src/cli/register_commands.cpp: an archive whose root the root of a
    // registration over a lost manifest overlaps on a volume that may be a
    // clone of this one (decision 0028, point 8); the match code is translated
    // before it is put in.
    {"possibly a clone: this root overlaps the root of archive {} ({}) on a volume that this one matches only as {}: "
        "this volume may be a sector-by-sector clone of that archive's, or that archive's own; confirm only where this "
        "root is not that archive's",
        "可能是複本：此根目錄與封存 {}（{}）的根目錄重疊，而此磁碟區與該封存的磁碟區比對只得到「{}」：此磁碟區可能是"
        "該封存的磁碟區逐磁區複製出來的複本，也可能就是該封存自己的磁碟區；請只在此根目錄不是該封存的根目錄時才確認",
        Match::FormatOnly},
    // An archive whose very root the root of a registration over a lost
    // manifest is, of which the archive named may be the twin (decision 0028,
    // point 8).
    {"the same root as archive {} ({}): this root is that archive's, on the volume and the device that its latest "
        "manifest records; confirm only where this very folder was registered on purpose as both that archive and "
        "this one (--force-new-archive)",
        "與封存 {}（{}）的根目錄相同：此根目錄是該封存的根目錄，位於它最新的清單檔所記錄的磁碟區與裝置上；請只在這個"
        "資料夾當初刻意同時註冊為該封存與此封存（--force-new-archive）時才確認",
        Match::FormatOnly},
    // The same on a volume that this one matches only as MIGRATED or
    // MATCH_WEAK, which may be that archive's or a clone of it; the match code
    // is translated before it is put in.
    {"the same root as archive {} ({}) on a volume that this one matches only as {}: this volume may be a "
        "sector-by-sector clone of that archive's, or that archive's own; confirm only where this very folder was "
        "registered on purpose as both that archive and this one (--force-new-archive), or where this volume is not "
        "that archive's",
        "與封存 {}（{}）的根目錄相同，而此磁碟區與該封存的磁碟區比對只得到「{}」：此磁碟區可能是該封存的磁碟區逐磁區"
        "複製出來的複本，也可能就是該封存自己的磁碟區；請只在這個資料夾當初刻意同時註冊為該封存與此封存"
        "（--force-new-archive），或此磁碟區不是該封存的磁碟區時才確認",
        Match::FormatOnly},
    // src/core/engine/abandon_registration.cpp
    {"archive {} has no unfinished registration", "封存 {} 沒有未完成的註冊"},
    {"the unfinished registration of archive {} is no longer the one shown (it was resumed, finished or "
        "abandoned meanwhile); nothing was changed",
        "封存 {} 未完成的註冊已不是畫面上顯示的那一個（期間已被繼續、完成或放棄）；未做任何變更"},
    {"the registration of archive {} is still running in process {} ({}) on this computer; "
        "abandon it once that process has ended",
        "封存 {} 的註冊仍在這台電腦的處理程序 {}（{}）中執行；請等該處理程序結束後再放棄"},
    {"an attempt of the registration of archive {} said it was working less than 3 minutes ago; "
        "abandon it once it has stopped",
        "封存 {} 的註冊在不到 3 分鐘前仍回報正在執行；請等它停止後再放棄"},
    {"cannot read the unfinished registration: {}", "無法讀取未完成的註冊：{}", Match::Cause},
    {"cannot abandon the registration: {}", "無法放棄註冊：{}", Match::Cause},
    {"the manifest copy on the media could not be written: {}", "無法寫入清單檔的媒體副本：{}",
        Match::Cause},
    {"{} entries carry portability warnings", "{} 個項目帶有可攜性警告"},
    // src/core/engine/verify_engine.cpp
    {"cannot list volumes: {}", "無法列出磁碟區：{}", Match::Cause},
    {"archive {} is not on any attached volume", "封存 {} 不在任何已連接的磁碟區上"},
    // A run resumed by its archive's uuid, whose archive more than one attached
    // volume holds (decision 0022, point 7): the roots are one value, quoted.
    {"AMBIGUOUS: archive {} is on more than one attached volume (cloned media): {}; run {} read \"{}\": resume it "
        "at the root on the volume it read with `verify --full <root_path> --resume {}`, or detach the other volumes "
        "and resume it again",
        "無法確定：封存 {} 位於多個已連接的磁碟區上（複製的媒體）：{}；執行 {} 讀取的是「{}」：請以 `verify --full "
        "<root_path> --resume {}` 從它讀取的那個磁碟區上的根目錄繼續，或讓其他磁碟區中斷連線後再繼續一次"},
    {"AMBIGUOUS: archive {} is on more than one attached volume (cloned media): {}; resume run {} at the root on "
        "the volume it read with `verify --full <root_path> --resume {}`, or detach the other volumes and resume it "
        "again",
        "無法確定：封存 {0} 位於多個已連接的磁碟區上（複製的媒體）：{1}；請以 `verify --full <root_path> --resume {3}` "
        "從執行 {2} 讀取的那個磁碟區上的根目錄繼續，或讓其他磁碟區中斷連線後再繼續一次"},
    // An archive named by its uuid, not to resume a run, that more than one
    // attached volume holds (decision 0022, point 7).
    {"AMBIGUOUS: archive {} is on more than one attached volume (cloned media): {}; verify it at the root on one of "
        "them with `verify --full <root_path>`, or detach the other volumes and verify it again",
        "無法確定：封存 {} 位於多個已連接的磁碟區上（複製的媒體）：{}；請以 `verify --full <root_path>` 驗證其中一個"
        "磁碟區上的根目錄，或讓其他磁碟區中斷連線後再驗證一次"},
    {"run {} is not an interrupted run", "執行 {} 不是中斷的執行"},
    // An interrupted attempt of a relocation, which `verify --resume` does not continue (decision 0026).
    {"run {} is an interrupted attempt of the unfinished relocation of archive {}; continue it with `archive "
        "relocate {} <root_path> --resume`",
        "執行 {} 是封存 {} 未完成的搬移中斷時留下的紀錄；請以 `archive relocate {} <root_path> --resume` 繼續"},
    {"run {} is an interrupted attempt of a relocation of archive {} that has ended; nothing is left to resume",
        "執行 {} 是封存 {} 某次搬移中斷時留下的紀錄，該搬移已結束；沒有可繼續的作業"},
    // A relocation made for an older revision never records (decision 0030).
    {"run {} is an interrupted attempt of the unfinished relocation of archive {}, which verifies revision {}, but "
        "the archive now has a later revision ({}); the relocation can no longer finish: abandon it with `archive "
        "abandon-relocation {}`, then relocate again",
        "執行 {} 是封存 {} 未完成的搬移中斷時留下的紀錄，該搬移驗證的是版次 {}，但封存現在已有較新的版次（{}）；"
        "該搬移再也無法完成：請以 `archive abandon-relocation {}` 放棄它，再重新搬移"},
    {"run {} has an invalid resume record", "執行 {} 的繼續紀錄無效"},
    {"media copy {} differs from the repository copy", "媒體副本 {} 與儲存庫中的副本不同"},
    {"the media holds revision {}, newer than the repository's {}: "
        "the repository may have been rolled back",
        "媒體上有版次 {}，比儲存庫的版次 {} 新：儲存庫可能已被回復到較舊的狀態"},
    {"no registered archive {}", "找不到已註冊的封存 {}"},
    {"UNKNOWN: this is not a registered archive; use `register` to create one",
        "未知：這不是已註冊的封存；請使用 `register` 建立封存"},
    {"AMBIGUOUS: the media matches several archives (cloned media); select one with `verify --full <archive_uuid>`; "
        "where the volumes of those archives are attached together, detach all but the volume of the one to verify, "
        "then verify it",
        "無法確定：此媒體符合多個封存（複製的媒體）；請以 `verify --full <archive_uuid>` 選擇其中一個；若這些封存的"
        "磁碟區同時連接著，請讓要驗證的那個封存以外的磁碟區都中斷連線，再驗證它"},
    {"MIGRATED: the filesystem matches archive {} but the device differs; "
        "confirm with `archive relocate {} <root_path>`",
        "已遷移：檔案系統符合封存 {}，但裝置不同；請以 `archive relocate {} <root_path>` 確認"},
    {"PREVIOUS_CARRIER: this media held an earlier revision of archive {} before it was "
        "relocated; it is not verified (decision 0004)",
        "先前的載體：此媒體在封存 {} 搬移前存放其較早的版次；不會驗證此媒體（決策紀錄 0004）"},
    {"the matched archive is missing from the database", "資料庫中找不到相符的封存"},
    {"run {} cannot be resumed: the media identity or the archive revision changed since it "
        "started; run a new verification",
        "執行 {} 無法繼續：自開始以來媒體身分或封存版次已改變；請執行新的驗證"},
    {"identity: no hardware serial to compare (MATCH_WEAK)",
        "媒體身分：沒有可比對的硬體序號（弱相符）"},
    {"identity: the filesystem id cannot be read; the volume was recognised by its device (MATCH_WEAK)",
        "媒體身分：讀不到檔案系統 UUID，改以硬碟序號和容量辨識（弱相符）"},
    {"scan: {}", "掃描：{}", Match::Cause},
    {"verification finished as PASSED but could not be recorded: {}",
        "驗證結果為通過，但無法記錄：{}",
        Match::Cause},
    {"verification finished as PASSED_WITH_WARNINGS but could not be recorded: {}",
        "驗證結果為通過（有警告），但無法記錄：{}",
        Match::Cause},
    {"verification finished as FAILED but could not be recorded: {}",
        "驗證結果為失敗，但無法記錄：{}",
        Match::Cause},
    {"verification finished as INCOMPLETE but could not be recorded: {}",
        "驗證結果為未完成，但無法記錄：{}",
        Match::Cause},
    // src/core/engine/candidates.cpp
    {"the root is not on the reported volume", "根目錄不在回報的磁碟區上"},
    // src/core/engine/comparison.cpp
    {"Unreadable(permission)", "無法讀取（權限不足）"},
    {"Unreadable(media)", "無法讀取（媒體錯誤）"},
    {"Unreadable(other)", "無法讀取（其他錯誤）"},
    {"mtime", "修改時間"},
    {"mode", "權限模式"},
    {"attrs", "檔案屬性"},
    {"mtime,mode", "修改時間、權限模式"},
    {"mtime,attrs", "修改時間、檔案屬性"},
    {"mode,attrs", "權限模式、檔案屬性"},
    {"mtime,mode,attrs", "修改時間、權限模式、檔案屬性"},
    {"link-target-translated", "連結目標已被轉換"},
    {"link target differs", "連結目標不同"},
    {"a mount point now covers this path", "此路徑現在被掛載點覆蓋"},
    {"directory {} cannot be listed", "無法列出資料夾 {} 的內容"},
    {"name cannot exist on {}", "此名稱無法存在於 {}"},
    {"name cannot exist on windows", "此名稱無法存在於 Windows"},
    {"name cannot exist on linux", "此名稱無法存在於 Linux"},
    {"inside a directory unreadable at registration", "位於註冊時無法讀取的資料夾內"},
    {"f -> d", "檔案 → 資料夾"},
    {"f -> l", "檔案 → 連結"},
    {"d -> f", "資料夾 → 檔案"},
    {"d -> l", "資料夾 → 連結"},
    {"l -> f", "連結 → 檔案"},
    {"l -> d", "連結 → 資料夾"},
    {"known-since-registration", "註冊時即無法讀取"},
    {"readable now, unreadable at registration", "現在可讀取，註冊時無法讀取"},
    {"size differs", "大小不同"},
    {"content differs", "內容不同"},
    // src/core/engine/compare_archives.cpp: its failures, and the item details
    // that verification does not use (it shares the others above). Each
    // "unreadable in A (<reason>)" is an entry of its own: a template would
    // carry the reason over in English.
    {"archive {} has no revision yet", "封存 {} 尚無版次"},
    {"cannot read the manifest of {} revision {}: {}", "無法讀取 {} 版次 {} 的清單檔：{}",
        Match::Cause},
    {"the comparison was cancelled", "比較已取消"},
    {"size {} -> {}", "大小 {} → {}"},
    {"unreadable in A (permission)", "A 註冊時無法讀取（權限不足）"},
    {"unreadable in A (media)", "A 註冊時無法讀取（媒體錯誤）"},
    {"unreadable in A (other)", "A 註冊時無法讀取（其他錯誤）"},
    {"unreadable in B (permission)", "B 註冊時無法讀取（權限不足）"},
    {"unreadable in B (media)", "B 註冊時無法讀取（媒體錯誤）"},
    {"unreadable in B (other)", "B 註冊時無法讀取（其他錯誤）"},
    {"unreadable in both archives", "兩個封存註冊時都無法讀取"},
    {"inside a directory unreadable in A", "位於 A 註冊時無法讀取的資料夾內"},
    {"inside a directory unreadable in B", "位於 B 註冊時無法讀取的資料夾內"},
    // src/core/engine/relocate_engine.cpp
    {"a retired archive takes no new revisions (decision 0003)",
        "已停用的封存不再接受新的版次（決策紀錄 0003）"},
    {"cannot identify the volume of the new root: {}", "無法識別新根目錄所在的磁碟區：{}",
        Match::Cause},
    {"the new root overlaps archive {} on the same volume (decision 0001)",
        "新根目錄與同一磁碟區上的封存 {} 重疊（決策紀錄 0001）"},
    {"ManifestHashMismatch: {}", "清單檔雜湊不符：{}", Match::Cause},
    {"ManifestRevisionRollback: {}", "清單檔版次倒退：{}", Match::Cause},
    {"RepositoryError: {}", "儲存庫錯誤：{}", Match::Cause},
    {"relocation was interrupted; nothing was changed", "搬移已中斷；未做任何變更"},
    // A relocation resumes (decision 0026).
    {"relocation was interrupted; continue it with `archive relocate {} <root_path> --resume`",
        "搬移已中斷；請以 `archive relocate {} <root_path> --resume` 繼續"},
    {"an unfinished relocation of archive {} (to \"{}\") exists; resume it with `archive relocate {} "
        "<root_path> --resume`, or abandon it with `archive abandon-relocation {}`",
        "封存 {} 有一個未完成的搬移（至「{}」）；請以 `archive relocate {} <root_path> --resume` 繼續，"
        "或以 `archive abandon-relocation {}` 放棄它"},
    {"archive {} has no unfinished relocation to resume", "封存 {} 沒有可繼續的未完成搬移"},
    {"revision {} of archive {} was recorded by another operation; the unfinished relocation can no longer "
        "finish, and its saved progress was removed",
        "封存 {1} 的版次 {0} 已由其他作業記錄；未完成的搬移再也無法完成，其已儲存的進度已刪除"},
    {"archive {} is at revision {}, not at revision {} that its unfinished relocation verifies; it cannot be "
        "resumed (abandon it with `archive abandon-relocation {}`)",
        "封存 {0} 目前是版次 {1}，不是其未完成的搬移所驗證的版次 {2}；無法繼續（請以 "
        "`archive abandon-relocation {3}` 放棄它）"},
    {"this relocation is still running in process {} ({}) on this computer; resume it once that process has "
        "ended",
        "此搬移仍在這台電腦的處理程序 {}（{}）中執行；請等該處理程序結束後再繼續"},
    {"the volume or folder of this root is not the one the unfinished relocation of archive {} started on "
        "(\"{}\"); resume it there, or abandon it with `archive abandon-relocation {}` and start again",
        "此根目錄所在的磁碟區或資料夾，不是封存 {} 未完成的搬移開始時的那一個（「{}」）；請在那裡繼續，"
        "或以 `archive abandon-relocation {}` 放棄後重新開始"},
    {"the unfinished relocation of archive {} (to \"{}\") found that the new carrier does not hold the recorded "
        "content and cannot be resumed; abandon it with `archive abandon-relocation {}`, then relocate again",
        "封存 {} 未完成的搬移（至「{}」）發現新載體上的內容與記錄不符，無法繼續；請以 "
        "`archive abandon-relocation {}` 放棄它，再重新搬移"},
    // A relocation made for an older revision never records (decision 0030).
    {"the unfinished relocation of archive {} (to \"{}\") verifies revision {}, but the archive now has a later "
        "revision ({}); the relocation can no longer finish: abandon it with `archive abandon-relocation {}`, then "
        "relocate again",
        "封存 {} 未完成的搬移（至「{}」）驗證的是版次 {}，但封存現在已有較新的版次（{}）；這次搬移再也無法完成："
        "請以 `archive abandon-relocation {}` 放棄它，再重新搬移"},
    {"revision {} of archive {} was recorded by another operation before this relocation could begin; nothing was "
        "changed (run `archive relocate` again)",
        "封存 {1} 的版次 {0} 已在這次搬移開始前由其他作業記錄；未做任何變更（請再執行一次 `archive relocate`）"},
    {"cannot start the relocation: {}", "無法開始搬移：{}", Match::Cause},
    {"the unfinished relocation of archive {} was abandoned while this attempt ran; nothing was changed",
        "封存 {} 未完成的搬移已在這次執行期間被放棄；未做任何變更"},
    {"an unfinished registration of archive {} (root \"{}\") is creating revision {}; this relocation recorded "
        "nothing (finish that registration with `register --resume`, then relocate again; or abandon it with "
        "`archive abandon-registration {}`, then continue this relocation with `archive relocate {} <root_path> "
        "--resume`)",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}；這次搬移未記錄任何內容（請以 "
        "`register --resume` 完成該註冊後再重新搬移；或以 `archive abandon-registration {}` 放棄該註冊後，"
        "以 `archive relocate {} <root_path> --resume` 繼續這次搬移）"},
    // src/core/engine/abandon_relocation.cpp
    {"archive {} has no unfinished relocation", "封存 {} 沒有未完成的搬移"},
    {"the unfinished relocation of archive {} is no longer the one shown (it was resumed, finished or "
        "abandoned meanwhile); nothing was changed",
        "封存 {} 未完成的搬移已不是畫面上顯示的那一個（期間已被繼續、完成或放棄）；未做任何變更"},
    {"the relocation of archive {} is still running in process {} ({}) on this computer; abandon it once that "
        "process has ended",
        "封存 {} 的搬移仍在這台電腦的處理程序 {}（{}）中執行；請等該處理程序結束後再放棄"},
    {"an attempt of the relocation of archive {} said it was working less than 3 minutes ago; abandon it once "
        "it has stopped",
        "封存 {} 的搬移在不到 3 分鐘前仍回報正在執行；請等它停止後再放棄"},
    {"cannot read the unfinished relocation: {}", "無法讀取未完成的搬移：{}", Match::Cause},
    {"cannot abandon the relocation: {}", "無法放棄搬移：{}", Match::Cause},
    {"cannot scan the new root: {}", "無法掃描新根目錄：{}", Match::Cause},
    // A failure that stops a relocation keeps its saved progress (decision 0026).
    {"cannot scan the new root: {}; the relocation keeps its saved progress: continue it with `archive relocate {} "
        "<root_path> --resume`",
        "無法掃描新根目錄：{}；搬移保留其已儲存的進度：請以 `archive relocate {} <root_path> --resume` 繼續",
        Match::Cause},
    {"cannot write the manifest: {}; the relocation keeps its saved progress: continue it with `archive relocate {} "
        "<root_path> --resume`",
        "無法寫入清單檔：{}；搬移保留其已儲存的進度：請以 `archive relocate {} <root_path> --resume` 繼續",
        Match::Cause},
    {"cannot record the relocation: {}; the relocation keeps its saved progress: continue it with `archive "
        "relocate {} <root_path> --resume`",
        "無法記錄搬移：{}；搬移保留其已儲存的進度：請以 `archive relocate {} <root_path> --resume` 繼續",
        Match::Cause},
    {"the new carrier does not hold the recorded content; "
        "nothing was changed (use `register --new-revision` to re-baseline changed content)",
        "新載體上的內容與記錄不符；未做任何變更（若內容已變更，"
        "請使用 `register --new-revision` 重新建立基準）"},
    {"the new carrier does not hold the recorded content; nothing was changed, but the relocation's saved "
        "progress could not be removed: {}; abandon the relocation with `archive abandon-relocation {}` before "
        "relocating again",
        "新載體上的內容與記錄不符；未做任何變更，但無法刪除搬移已儲存的進度：{}；請先以 "
        "`archive abandon-relocation {}` 放棄這次搬移，再重新搬移",
        Match::Cause},
    {"cannot record the relocation: {}", "無法記錄搬移：{}", Match::Cause},
    {"revision {} of archive {} was recorded by another operation while this relocation ran; "
        "nothing was changed",
        "封存 {1} 的版次 {0} 已在這次搬移期間由其他作業記錄；未做任何變更"},
    // Also register_engine.cpp's, for a registration of the archive through another root path (decision 0025).
    {"an unfinished registration of archive {} (root \"{}\") is creating revision {}; "
        "nothing was changed (finish it with `register --resume` first, or abandon it with "
        "`archive abandon-registration {}`)",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}；未做任何變更"
        "（請先以 `register --resume` 完成該註冊，或以 `archive abandon-registration {}` 放棄它）"},
    // src/core/engine/transfer.cpp
    {"cannot compare with the stored manifest", "無法與已儲存的清單檔比較"},
    {"revision {} is already known with different bytes", "版次 {} 已存在，但位元組內容不同"},
    {"revision {} is older than the known revision {}; "
        "pass --force-rollback to import it anyway",
        "版次 {} 比已知的版次 {} 舊；若仍要匯入，請加上 --force-rollback"},
    // An imported manifest gets its name in the transaction that records it (decision 0030).
    {"the manifest changed while it was imported; nothing was imported", "清單檔在匯入期間被變更；未匯入任何內容"},
    {"revision {} of archive {} was recorded by another operation while this import ran; nothing was imported "
        "(import it again to compare it with the recorded manifest)",
        "封存 {1} 的版次 {0} 已在這次匯入期間由其他作業記錄；未匯入任何內容（再匯入一次，即可與已記錄的清單檔比較）"},
    {"an unfinished registration of archive {} (root \"{}\") is creating revision {}; nothing was imported "
        "(finish that registration with `register --resume`, or abandon it with `archive abandon-registration {}` "
        "and import again)",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}；未匯入任何內容（請以 `register --resume` "
        "完成該註冊，或以 `archive abandon-registration {}` 放棄它後再匯入一次）"},
    {"the repository's record of revision {} of archive {} changed while this import ran; nothing was imported "
        "(import it again)",
        "儲存庫中封存 {1} 版次 {0} 的紀錄已在這次匯入期間變更；未匯入任何內容（請再匯入一次）"},
    // src/core/engine/recorded_manifest.cpp
    {"revision {} is not recorded", "版次 {} 沒有紀錄"},
    {"the manifest differs from the repository record", "清單檔與儲存庫的紀錄不符"},
    {"revision {} is older than the known revision {}", "版次 {} 比已知的版次 {} 舊"},
    // A manifest that cannot be used, and how to fix it (decision 0028). The
    // index's copy of a header is never taken for the manifest's (R-14). One
    // that is missing, cannot be read or has changed is put back with `import`
    // of its file (point 7); for the archive's latest revision the message
    // ends with the last resort, a registration over its lost manifest (point
    // 8), and, where the device is gone as well, retiring the archive (point 9).
    {"the manifest of revision {} ({}) is missing; put it back with `import <folder>/{}` from a backup of the "
        "repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one; if none of these holds it, register revision {} from what the archive's root holds "
        "now with `register <root_path> --lost-manifest {}`; if the device that held the archive is gone as well, "
        "retire the archive with `archive retire {} --confirm`",
        "版次 {0} 的清單檔（{1}）已遺失；請以 `import <folder>/{2}` 從儲存庫的備份、版次 {3} 的匯出檔，或封存 "
        ".veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 `register <root_path> "
        "--lost-manifest {5}` 用封存根目錄目前的內容註冊版次 {4}；若存放此封存的裝置也已不在，請以 "
        "`archive retire {6} --confirm` 停用此封存"},
    {"the manifest of revision {} ({}) cannot be read: {}; put it back with `import <folder>/{}` from a backup of "
        "the repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one; if none of these holds it, register revision {} from what the archive's root holds "
        "now with `register <root_path> --lost-manifest {}`; if the device that held the archive is gone as well, "
        "retire the archive with `archive retire {} --confirm`",
        "無法讀取版次 {0} 的清單檔（{1}）：{2}；請以 `import <folder>/{3}` 從儲存庫的備份、版次 {4} 的匯出檔，"
        "或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 `register <root_path> "
        "--lost-manifest {6}` 用封存根目錄目前的內容註冊版次 {5}；若存放此封存的裝置也已不在，請以 "
        "`archive retire {7} --confirm` 停用此封存",
        Match::Cause},
    {"the manifest of revision {} ({}) has changed: {}; put it back with `import <folder>/{}` from a backup of the "
        "repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one; if none of these holds it, register revision {} from what the archive's root holds "
        "now with `register <root_path> --lost-manifest {}`; if the device that held the archive is gone as well, "
        "retire the archive with `archive retire {} --confirm`",
        "版次 {0} 的清單檔（{1}）已被變更：{2}；請以 `import <folder>/{3}` 從儲存庫的備份、版次 {4} 的匯出檔，"
        "或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 `register <root_path> "
        "--lost-manifest {6}` 用封存根目錄目前的內容註冊版次 {5}；若存放此封存的裝置也已不在，請以 "
        "`archive retire {7} --confirm` 停用此封存",
        Match::Cause},
    // The same of the latest revision of a retired archive, which takes no new
    // revision: the message ends with the import (point 9).
    {"the manifest of revision {} ({}) is missing; put it back with `import <folder>/{}` from a backup of the "
        "repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one",
        "版次 {0} 的清單檔（{1}）已遺失；請以 `import <folder>/{2}` 從儲存庫的備份、版次 {3} 的匯出檔，或封存 "
        ".veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔"},
    {"the manifest of revision {} ({}) cannot be read: {}; put it back with `import <folder>/{}` from a backup of "
        "the repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one",
        "無法讀取版次 {0} 的清單檔（{1}）：{2}；請以 `import <folder>/{3}` 從儲存庫的備份、版次 {4} 的匯出檔，"
        "或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔",
        Match::Cause},
    {"the manifest of revision {} ({}) has changed: {}; put it back with `import <folder>/{}` from a backup of the "
        "repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one",
        "版次 {0} 的清單檔（{1}）已被變更：{2}；請以 `import <folder>/{3}` 從儲存庫的備份、版次 {4} 的匯出檔，"
        "或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔",
        Match::Cause},
    {"the repository's index has no record of the manifest of revision {} ({}), which is missing; put it back with "
        "`import <folder>/{}` from a backup of the repository, an export of revision {}, or the media copy in the "
        "archive's .veriarchive folder, where registration wrote one",
        "儲存庫的索引沒有版次 {0} 的清單檔（{1}）的紀錄，而該清單檔已遺失；請以 `import <folder>/{2}` 從儲存庫的"
        "備份、版次 {3} 的匯出檔，或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔"},
    {"the repository's index has no record of the manifest of revision {} ({}), which cannot be read: {}; put it "
        "back with `import <folder>/{}` from a backup of the repository, an export of revision {}, or the media copy "
        "in the archive's .veriarchive folder, where registration wrote one",
        "儲存庫的索引沒有版次 {0} 的清單檔（{1}）的紀錄，而且無法讀取該清單檔：{2}；請以 `import <folder>/{3}` "
        "從儲存庫的備份、版次 {4} 的匯出檔，或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔",
        Match::Cause},
    // The same of an earlier revision, which has a later revision already:
    // where nothing holds its manifest, `repo rebuild-index` leaves it out of
    // the index (point 6).
    {"the manifest of revision {} ({}) is missing; put it back with `import <folder>/{}` from a backup of the "
        "repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one; if none of these holds it, leave revision {} out of the index with `repo "
        "rebuild-index`",
        "版次 {} 的清單檔（{}）已遺失；請以 `import <folder>/{}` 從儲存庫的備份、版次 {} 的匯出檔，或封存 "
        ".veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 `repo rebuild-index` 將版次 {} "
        "從索引移除"},
    {"the manifest of revision {} ({}) cannot be read: {}; put it back with `import <folder>/{}` from a backup of "
        "the repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one; if none of these holds it, leave revision {} out of the index with `repo "
        "rebuild-index`",
        "無法讀取版次 {} 的清單檔（{}）：{}；請以 `import <folder>/{}` 從儲存庫的備份、版次 {} 的匯出檔，或封存 "
        ".veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 `repo rebuild-index` 將版次 {} "
        "從索引移除",
        Match::Cause},
    {"the manifest of revision {} ({}) has changed: {}; put it back with `import <folder>/{}` from a backup of the "
        "repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, where "
        "registration wrote one; if none of these holds it, leave revision {} out of the index with `repo "
        "rebuild-index`",
        "版次 {} 的清單檔（{}）已被變更：{}；請以 `import <folder>/{}` 從儲存庫的備份、版次 {} 的匯出檔，或封存 "
        ".veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，請以 `repo rebuild-index` 將版次 {} "
        "從索引移除",
        Match::Cause},
    {"the repository's index differs from the manifest of revision {} ({}); rebuild the index with "
        "`repo rebuild-index`",
        "儲存庫的索引與版次 {} 的清單檔（{}）不符；請以 `repo rebuild-index` 重建索引"},
    {"its first line differs from what the repository recorded", "其第一行與儲存庫所記錄的不同"},
    {"its body does not match its header", "其主體與標頭不符"},
    {"it gave other bytes when it was read again", "再次讀取時得到不同的內容"},
    // The manifest of the archive's revision of which the index has no record,
    // as where `repo rebuild-index` could not read it (decision 0028). The
    // import that puts it back records it.
    {"the repository's index has no record of the manifest of revision {} ({}), which is missing; put it back with "
        "`import <folder>/{}` from a backup of the repository, an export of revision {}, or the media copy in the "
        "archive's .veriarchive folder, where registration wrote one; if none of these holds it, register revision "
        "{} from what the archive's root holds now with `register <root_path> --lost-manifest {}`; if the device that "
        "held the archive is gone as well, retire the archive with `archive retire {} --confirm`",
        "儲存庫的索引沒有版次 {0} 的清單檔（{1}）的紀錄，而該清單檔已遺失；請以 `import <folder>/{2}` 從儲存庫的"
        "備份、版次 {3} 的匯出檔，或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；若這些都沒有，"
        "請以 `register <root_path> --lost-manifest {5}` 用封存根目錄目前的內容註冊版次 {4}；若存放此封存的裝置"
        "也已不在，請以 `archive retire {6} --confirm` 停用此封存"},
    {"the repository's index has no record of the manifest of revision {} ({}), which cannot be read: {}; put it "
        "back with `import <folder>/{}` from a backup of the repository, an export of revision {}, or the media copy "
        "in the archive's .veriarchive folder, where registration wrote one; if none of these holds it, register "
        "revision {} from what the archive's root holds now with `register <root_path> --lost-manifest {}`; if the "
        "device that held the archive is gone as well, retire the archive with `archive retire {} --confirm`",
        "儲存庫的索引沒有版次 {0} 的清單檔（{1}）的紀錄，而且無法讀取該清單檔：{2}；請以 `import <folder>/{3}` "
        "從儲存庫的備份、版次 {4} 的匯出檔，或封存 .veriarchive 資料夾中註冊時寫入的媒體副本放回該清單檔；"
        "若這些都沒有，請以 `register <root_path> --lost-manifest {6}` 用封存根目錄目前的內容註冊版次 {5}；"
        "若存放此封存的裝置也已不在，請以 `archive retire {7} --confirm` 停用此封存",
        Match::Cause},
    {"the repository's index has no record of the manifest of revision {} ({}); rebuild the index with "
        "`repo rebuild-index`",
        "儲存庫的索引沒有版次 {} 的清單檔（{}）的紀錄；請以 `repo rebuild-index` 重建索引"},
    // What is wrong alone, inside a verification by root that stops (decision 0028).
    {"the manifest of revision {} ({}) is missing", "版次 {} 的清單檔（{}）已遺失"},
    {"the manifest of revision {} ({}) cannot be read: {}", "無法讀取版次 {} 的清單檔（{}）：{}", Match::Cause},
    {"the manifest of revision {} ({}) has changed: {}", "版次 {} 的清單檔（{}）已被變更：{}", Match::Cause},
    {"the repository's index differs from the manifest of revision {} ({})", "儲存庫的索引與版次 {} 的清單檔（{}）不符"},
    {"the repository's index has no record of the manifest of revision {} ({}), which is missing",
        "儲存庫的索引沒有版次 {} 的清單檔（{}）的紀錄，而該清單檔已遺失"},
    {"the repository's index has no record of the manifest of revision {} ({}), which cannot be read: {}",
        "儲存庫的索引沒有版次 {} 的清單檔（{}）的紀錄，而且無法讀取該清單檔：{}", Match::Cause},
    {"the repository's index has no record of the manifest of revision {} ({})",
        "儲存庫的索引沒有版次 {} 的清單檔（{}）的紀錄"},
    // src/core/engine/verify_engine.cpp: by root, the archive whose manifest
    // cannot be used may be the one at the root (decision 0028): verify it by
    // its uuid, then the steps of the plain message, the manifest put back
    // (which records it where the index has no record of it), the last resort
    // and the retirement where the device is gone as well, or the index
    // rebuilt where only it is wrong.
    {"this root may belong to archive {} ({}), whose manifest cannot be used: {}; verify that archive by its uuid "
        "(`verify --full {}`) to record the failure, then put its manifest back with `import <folder>/{}` from a "
        "backup of the repository, an export of revision {}, or the media copy in the archive's .veriarchive folder, "
        "where registration wrote one; if none of these holds it, register revision {} from what the archive's root "
        "holds now with `register <root_path> --lost-manifest {}`; if the device that held the archive is gone as "
        "well, retire the archive with `archive retire {} --confirm`",
        "此根目錄可能屬於封存 {0}（{1}），而它的清單檔無法使用：{2}；請先以 uuid 驗證該封存（`verify --full {3}`）以記錄這次"
        "失敗，再以 `import <folder>/{4}` 從儲存庫的備份、版次 {5} 的匯出檔，或封存 .veriarchive 資料夾中註冊時寫入的媒體"
        "副本放回它的清單檔；若這些都沒有，請以 `register <root_path> --lost-manifest {7}` 用封存根目錄目前的內容註冊版次 {6}"
        "；若存放此封存的裝置也已不在，請以 `archive retire {8} --confirm` 停用此封存",
        Match::Cause},
    {"this root may belong to archive {} ({}), whose manifest cannot be used: {}; verify that archive by its uuid "
        "(`verify --full {}`) to record the failure, then rebuild the index with `repo rebuild-index`",
        "此根目錄可能屬於封存 {}（{}），而它的清單檔無法使用：{}；請先以 uuid 驗證該封存（`verify --full {}`）以記錄這次失敗，"
        "再以 `repo rebuild-index` 重建索引",
        Match::Cause},
    // src/core/engine/rebuild_index.cpp
    {"cannot list the manifests of archive {}: {}", "無法列出封存 {} 的清單檔：{}", Match::Cause},
    {"cannot read manifest {}/{}: {}", "無法讀取清單檔 {}/{}：{}", Match::Cause},
    {"header does not match its file name", "標頭與其檔名不符"},
    // src/common/config.cpp
    {"Cannot read config", "無法讀取設定檔"},
    {"Config must be an object", "設定檔必須是 JSON 物件"},
    {"Repository path is empty", "儲存庫路徑是空的"},
    // Config keys stay as written in the file (decision 0017).
    {"Policy must be an object", "設定檔中的 policy 必須是 JSON 物件"},
    {"Trusted keys must be an array", "設定檔中的 trusted_keys 必須是 JSON 陣列"},
    {"Invalid config: {}", "設定檔無效：{}", Match::Cause},
    // src/common/archive_path.cpp
    {"ArchivePath must be relative", "ArchivePath 必須是相對路徑"},
    {"ArchivePath cannot contain ..", "ArchivePath 不可包含 .."},
    {"name was recorded with another platform's encoding: {}", "名稱是以其他平台的編碼記錄的：{}"},
    {"invalid raw ArchivePath", "無效的原始 ArchivePath"},
    {"invalid ArchivePath", "無效的 ArchivePath"},
    // src/core/repository/repository.cpp
    {"cannot write repository.json: {}", "無法寫入 repository.json：{}", Match::Cause},
    {"cannot create repository directory: {}", "無法建立儲存庫資料夾：{}", Match::Cause},
    {"cannot inspect repository schema", "無法檢查儲存庫的結構描述"},
    {"cannot inspect repository tables", "無法檢查儲存庫的資料表"},
    {"Explicit config does not exist", "指定的設定檔不存在"},
    {"No repository configured; run repo init first", "尚未設定儲存庫；請先執行 repo init"},
    {"no writable repository location", "沒有可寫入的儲存庫位置"},
    {"configured repository is unavailable or is not a VeriArchive repository",
        "設定的儲存庫無法使用，或不是 VeriArchive 儲存庫"},
    {"not a VeriArchive repository: invalid repository.json",
        "不是 VeriArchive 儲存庫：repository.json 無效"},
    {"cannot create a repository at the given path", "無法在指定的路徑建立儲存庫"},
    {"SQLite on a network filesystem is unsafe (locking, WAL); "
        "set allow_network_repository or move the repository",
        "在網路檔案系統上使用 SQLite 並不安全（鎖定、WAL）；請設定 allow_network_repository，"
        "或將儲存庫移到其他位置"},
    {"cannot pin repository in config: {}", "無法在設定檔中固定儲存庫：{}", Match::Cause},
    {"Invalid repository.json", "repository.json 無效"},
    {"cannot record repository_uuid in config: {}", "無法在設定檔中記錄 repository_uuid：{}",
        Match::Cause},
    {"unsupported repository schema", "不支援的儲存庫結構描述"},
    {"unsupported or incomplete repository schema", "不支援或不完整的儲存庫結構描述"},
    // src/core/repository/sqlite_raii.cpp
    {"unknown SQLite error", "未知的 SQLite 錯誤"},
    {"cannot open repository database: {}", "無法開啟儲存庫資料庫：{}", Match::Cause},
    {"cannot execute SQL: {}", "無法執行 SQL：{}", Match::Cause},
    {"cannot prepare SQL: {}", "無法準備 SQL 陳述式：{}", Match::Cause},
    {"cannot run SQL: {}", "無法執行 SQL 陳述式：{}", Match::Cause},
    // SQLite's own messages (sqlite3_errmsg), the cause in the messages above.
    // Exact texts of the linked SQLite (3.53).
    {"database is locked", "資料庫已被鎖定"},
    {"database table is locked", "資料表已被鎖定"},
    {"attempt to write a readonly database", "無法寫入唯讀的資料庫"},
    {"disk I/O error", "磁碟 I/O 錯誤"},
    {"database disk image is malformed", "資料庫檔案已損毀"},
    {"database or disk is full", "資料庫或磁碟已滿"},
    {"unable to open database file", "無法開啟資料庫檔案"},
    {"file is not a database", "檔案不是資料庫"},
    {"constraint failed", "違反限制條件"},
    {"SQL logic error", "SQL 邏輯錯誤"},
    {"out of memory", "記憶體不足"},
    {"interrupted", "已中斷"},
    {"database schema has changed", "資料庫結構描述已變更"},
    {"cannot start a transaction within a transaction", "無法在交易中再開始交易"},
    {"cannot commit - no transaction is active", "無法提交：沒有進行中的交易"},
    // These start with literal text, so they are tried before "{} failed: {}"
    // and the column names after ": " stay as they are.
    {"UNIQUE constraint failed: {}", "違反唯一性限制：{}"},
    {"NOT NULL constraint failed: {}", "違反非空值限制：{}"},
    {"no such table: {}", "找不到資料表：{}"},
    // src/core/manifest/manifest.cpp
    {"not a version 1 .vam manifest", "不是格式版本 1 的 .vam 清單檔"},
    {"unknown entry type {}", "未知的項目類型 {}"},
    // src/core/manifest/manifest_reader.cpp
    {"manifest does not end with a newline", "清單檔未以換行字元結尾"},
    {"empty manifest", "清單檔是空的"},
    {"invalid manifest header: {}", "清單檔標頭無效：{}", Match::Cause},
    {"manifest body does not match its header (ManifestHashMismatch)",
        "清單檔主體與其標頭不符（清單檔雜湊不符）"},
    {"the manifest changed while it was read (ManifestHashMismatch)", "清單檔在讀取期間被變更（清單檔雜湊不符）"},
    {"invalid manifest entry: {}", "清單檔項目無效：{}", Match::Cause},
    {"manifest entries out of canonical order", "清單檔項目未依標準順序排列"},
    // nlohmann::json's sentences (common/json_error.h removes the library's tag),
    // the cause in the config and manifest messages above. Type names (string,
    // number, object, array) and key names are JSON terms and stay as they are.
    {"key '{}' not found", "找不到欄位「{}」"},
    {"type must be {}, but is {}", "類型必須是 {}，實際是 {}"},
    {"cannot use at() with {}", "需要 JSON 物件，實際是 {}"},
    {"cannot use value() with {}", "需要 JSON 物件，實際是 {}"},
    {"parse error at line {}, column {}: {}", "JSON 語法錯誤（第 {} 行，第 {} 欄）：{}", Match::Cause},
    {"attempting to parse an empty input; check that your input string or stream contains the "
        "expected JSON",
        "內容是空的，沒有 JSON 資料"},
    // src/core/manifest/manifest_writer.cpp
    {"manifest entries out of canonical order at {}", "清單檔項目在 {} 處未依標準順序排列"},
    {"manifest body changed while writing", "清單檔主體在寫入期間發生變更"},
    // src/core/identity/matcher.cpp
    {"volume-label-differs", "磁碟區標籤不同"},
    {"capacity-differs", "容量不同"},
    {"filesystem-type-differs", "檔案系統類型不同"},
    {"model-differs", "型號不同"},
    {"device-replaced", "裝置已更換"},
    {"filesystem-id-unavailable", "讀不到檔案系統 UUID"},
    // Shown as "identity: retired": the archive is retired, not the identity.
    {"retired", "封存已停用"},
    {"identity-file-copied", "身分檔案是從其他媒體複製而來"},
    // src/core/hash/hashers.cpp
    {"cannot initialise SHA-256", "無法初始化 SHA-256"},
    // src/platform/windows/win_util.cpp
    {"{} failed (Win32 error {})", "{} 失敗（Win32 錯誤 {}）"},
    // src/platform/windows/platform_services_win.cpp
    {"GetModuleFileNameW failed", "GetModuleFileNameW 失敗"},
    {"USERPROFILE is unavailable", "無法取得 USERPROFILE"},
    // src/platform/windows/fs_access_win.cpp
    {"short reparse buffer", "重新分析點資料過短"},
    {"short WSL symlink", "WSL 符號連結資料過短"},
    {"unsupported link type", "不支援的連結類型"},
    // src/platform/windows/file_writer_win.cpp
    {"cannot create temporary file", "無法建立暫存檔案"},
    // src/platform/linux/posix_util.cpp
    {"{} failed: {}", "{} 失敗：{}", Match::Cause},
    // strerror() texts, the cause after "<call> failed: ". The CLI never calls
    // setlocale, so glibc gives these English texts (where GTK has set a locale,
    // glibc may already give its own translation, which is shown as it is).
    // Windows' own messages come in the system language; std::filesystem errors
    // on Windows may give the same English texts ("Filename too long" is UCRT's).
    {"Operation not permitted", "不允許此操作"},
    {"No such file or directory", "找不到檔案或資料夾"},
    {"Input/output error", "輸入/輸出錯誤"},
    {"Permission denied", "權限不足"},
    {"Device or resource busy", "裝置或資源忙碌中"},
    {"File exists", "檔案已存在"},
    {"No such device", "找不到裝置"},
    {"No such device or address", "找不到裝置或位址"},
    {"Not a directory", "不是資料夾"},
    {"Is a directory", "這是資料夾"},
    {"Directory not empty", "資料夾不是空的"},
    {"Invalid argument", "無效的引數"},
    {"Too many open files", "開啟的檔案過多"},
    {"No space left on device", "裝置上沒有剩餘空間"},
    {"Read-only file system", "檔案系統是唯讀的"},
    {"Disk quota exceeded", "超過磁碟配額"},
    {"No medium found", "找不到媒體"},
    {"Connection timed out", "連線逾時"},
    {"Stale file handle", "檔案控制代碼已失效"},
    {"Transport endpoint is not connected", "傳輸端點未連線"},
    {"Cannot send after transport endpoint shutdown", "傳輸端點已關閉"},
    {"Too many levels of symbolic links", "符號連結層數過多"},
    {"File name too long", "檔名過長"},
    {"Filename too long", "檔名過長"},
    {"Operation not supported", "不支援此操作"},
    {"Operation canceled", "操作已取消"},
    {"Function not implemented", "功能未實作"},
    // On Linux file systems EBADMSG is a checksum failure and EUCLEAN a
    // corrupted file system; the literal wording would not say so.
    {"Bad message", "資料損毀（校驗碼錯誤）"},
    {"Structure needs cleaning", "檔案系統結構損毀，需要修復"},
    // src/platform/linux/platform_services_linux.cpp
    {"readlink failed", "readlink 失敗"},
    {"HOME is unavailable", "無法取得 HOME"},
    // src/platform/linux/fs_access_linux.cpp
    {"not a regular file", "不是一般檔案"},
    // src/platform/linux/media_enumerator_linux.cpp
    {"cannot read /proc/self/mountinfo", "無法讀取 /proc/self/mountinfo"},
    {"no mount contains the path", "沒有任何掛載點包含此路徑"},
};

} // namespace

std::span<const CatalogEntry> traditional_chinese_catalog()
{
    return entries;
}

} // namespace va::app
