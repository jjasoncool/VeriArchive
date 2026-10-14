// Traditional Chinese catalog (decision 0017). Each entry pairs the English
// text written in the code with its translation; "{}" marks a value and must
// appear the same number of times on both sides ("{0}", "{1}" when the Chinese
// word order differs). Terms follow the table in decision 0017.
//
// An entry may add a match mode (text.h): Match::Cause for a wrapper whose last
// value is an engine message ("context: {}"), Match::FormatOnly for a template
// used only through Text::format. Without one, values are carried over as they
// are. Library and system messages (SQLite, JSON, strerror) have entries too:
// they arrive as the cause inside engine messages.
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
        "           [--force-new-archive] [--copy-of UUID] [--write-media-copy] [--resume]\n"
        "  verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd]\n"
        "         [--resume RUN_ID]\n"
        "  archive list | show <uuid> | retire <uuid> | relocate <uuid> <root_path>\n"
        "  history <uuid>\n"
        "  export-sha256sums <uuid> --out FILE\n"
        "  export <uuid> --out DIR | import <file.vam> [--force-rollback]\n",
        "用法：veriarchive-cli [--config P] [--repository P] [--lang en|zh-TW] [--json] <command>\n"
        "\n"
        "  repo init | info | rebuild-index | verify-history\n"
        "  media list\n"
        "  register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision]\n"
        "           [--force-new-archive] [--copy-of UUID] [--write-media-copy] [--resume]\n"
        "  verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd]\n"
        "         [--resume RUN_ID]\n"
        "  archive list | show <uuid> | retire <uuid> | relocate <uuid> <root_path>\n"
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
    {"hashing: {}/{} files, {} MiB", "計算雜湊中：{}/{} 個檔案，{} MiB"},
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
    {"V1 detects edited or deleted rows; signed checkpoints arrive in V1.1.",
        "V1 可偵測被修改或刪除的紀錄；簽章檢查點將於 V1.1 提供。"},
    {"History BROKEN at run {}: {}", "歷程於執行 {} 處斷裂：{}"},
    {"previous row was deleted or edited (prev_hash does not link)",
        "前一筆紀錄已被刪除或修改（prev_hash 無法銜接）"},
    {"row was edited (row_hash does not match its contents)",
        "此筆紀錄已被修改（row_hash 與其內容不符）"},
    {"Rebuilt the index from manifests: {} archives, {} revisions, {} entries.",
        "已從清單檔重建索引：{} 個封存、{} 個版次、{} 個項目。"},
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
        "[--force-new-archive] [--copy-of UUID] [--write-media-copy] [--resume]",
        "用法：register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision] "
        "[--force-new-archive] [--copy-of UUID] [--write-media-copy] [--resume]"},
    {"--io takes hdd or ssd", "--io 只接受 hdd 或 ssd"},
    {"not a directory: {}", "不是資料夾：{}"},
    // src/cli/verify_commands.cpp
    {"FULL VERIFY {}  {} ({}) revision {}", "完整驗證 {}  {}（{}）版次 {}"},
    {"identity: {}", "媒體身分：{}", Match::Cause},
    {"coverage: {} of {} registered bytes read and matched",
        "涵蓋範圍：已註冊的 {1} 位元組中，有 {0} 位元組已讀取且相符"},
    {"items:", "項目："},
    {"failure: {}", "失敗：{}"},
    {"resume with: veriarchive-cli verify --full {} --resume {}",
        "若要繼續，請執行：veriarchive-cli verify --full {} --resume {}"},
    {"resumed from run: {}", "已從執行 {} 繼續"},
    {"run: {}", "執行：{}"},
    {"V1 detects accidental corruption and unintended change; "
        "tamper evidence (signed manifests) begins in V1.1.",
        "V1 可偵測意外損毀與非預期的變更；竄改證據（簽章清單檔）自 V1.1 起提供。"},
    {"QUICK and DEEP verification arrive in V1.1; use --full",
        "快速驗證與深度驗證將於 V1.1 提供；請使用 --full"},
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
    {"Retired {} ({}). It is no longer expected to be verified, "
        "is still recognised when attached, and keeps its history.",
        "已停用 {}（{}）。此封存不再需要定期驗證，接上時仍會被辨識，並保留其歷程。"},
    {"RELOCATION REFUSED: {}", "拒絕搬移：{}"},
    {"Relocated {} ({}) to {} as revision {}", "已將 {}（{}）搬移至 {}，成為版次 {}"},
    {"FULL VERIFY {} of the new carrier (run {})", "新載體的完整驗證：{}（執行 {}）"},
    {"never verified", "從未驗證"},
    {"{}  {} VERIFY {}  r{}  {} bytes", "{}  {} {}  r{}  {} 位元組", Match::FormatOnly},
    {"several archives start with {}", "有多個封存以 {} 開頭"},
    {"no archive {}", "找不到封存 {}"},
    {"usage: archive list | show <uuid> | retire <uuid> | relocate <uuid> <root_path> "
        "[--io hdd|ssd]",
        "用法：archive list | show <uuid> | retire <uuid> | relocate <uuid> <root_path> "
        "[--io hdd|ssd]"},
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

    // Graphical interface
    // src/gui/gui_app.cpp
    {"Archive List", "封存列表"},
    {"Register Archive", "註冊封存"},
    {"Verify Archive", "驗證封存"},
    {"Verification History", "驗證歷程"},
    {"Settings", "設定"},
    // src/gui/gui_support.cpp
    {"Scanning: {} entries", "掃描中：{} 個項目"},
    {"Hashing: {} / {} files, {}", "計算雜湊中：{} / {} 個檔案，{}"},
    {"Starting...", "正在啟動……"},
    {"Unexpected error: {}", "未預期的錯誤：{}"},
    // src/gui/archive_list_view.cpp
    {"never", "從未"},
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
    // src/gui/history_view.cpp
    {"Finished", "完成時間"},
    {"Mode", "模式"},
    {"Result", "結果"},
    {"Revision", "版次"},
    {"Bytes matched", "相符位元組"},
    {"Failures", "失敗類別"},
    {"Run", "執行"},
    {"Never verified.", "從未驗證。"},
    {"Cannot check the history: {}", "無法檢查歷程：{}"},
    {"History intact: {} runs, hash chain verified. (V1 detects edited or deleted rows; "
        "signed checkpoints arrive in V1.1.)",
        "歷程完整：{} 次執行，雜湊鏈已驗證。（V1 可偵測被修改或刪除的紀錄；"
        "簽章檢查點將於 V1.1 提供。）"},
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
    {"Trusted signing keys: signed manifests arrive in V1.1.",
        "受信任的簽章金鑰：簽章清單檔將於 V1.1 提供。"},
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
    {"Another operation is running.", "已有其他作業正在執行。"},
    {"Open a repository first (Settings).", "請先開啟儲存庫（見「設定」）。"},
    {"Choose the folder to register.", "請選擇要註冊的資料夾。"},
    {"Registering {} ...", "正在註冊 {}……"},
    {"Cannot open the repository: {}", "無法開啟儲存庫：{}"},
    {"Not registered: {}", "未能註冊：{}"},
    {"Done", "完成"},
    {"Registration reads every file of the folder and records its fingerprints (BLAKE3 and "
        "SHA-256) as the baseline that later verifications compare against. "
        "Nothing is written to the archive unless you ask for a media copy.",
        "註冊會讀取資料夾中的每個檔案，並記錄其指紋（BLAKE3 與 SHA-256）作為基準，"
        "供日後驗證比對。除非您要求寫入媒體副本，否則不會對封存寫入任何內容。"},
    {"Archive folder or volume root", "封存資料夾或磁碟區根目錄"},
    {"Choose...", "選擇…"},
    {"Choose the archive folder", "選擇封存資料夾"},
    {"Label (optional, e.g. ARCH-0012)", "標籤（選填，例如 ARCH-0012）"},
    {"New revision of the archive registered at this folder", "為此資料夾已註冊的封存建立新版次"},
    {"Register as a separate archive even though this folder is registered",
        "即使此資料夾已註冊，仍註冊為另一個封存"},
    {"Also write a manifest copy to the media (.veriarchive)",
        "同時將清單檔寫入媒體作為媒體副本（.veriarchive）"},
    {"Resume an interrupted registration of this folder", "繼續此資料夾中斷的註冊"},
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
    {"Failure: {}", "失敗：{}"},
    {"The run was interrupted; use Resume to continue without re-reading verified files.",
        "此次執行已中斷；使用「繼續中斷的執行」即可接續，不必重新讀取已驗證的檔案。"},
    {"Run: {}", "執行：{}"},
    {"Choose the folder to verify, or pick a registered archive.",
        "請選擇要驗證的資料夾，或挑選已註冊的封存。"},
    {"Verifying (reading every registered byte) ...", "正在驗證（讀取每個已註冊的位元組）……"},
    {"Not verified: {}", "未能驗證：{}"},
    {"The folder above", "上方的資料夾"},
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
    {"an interrupted registration of this root exists (archive {}); finish it with --resume",
        "此根目錄有一個中斷的註冊（封存 {}）；請以 --resume 完成它"},
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
    {"the manifest copy on the media could not be written: {}", "無法寫入清單檔的媒體副本：{}",
        Match::Cause},
    {"{} entries carry portability warnings", "{} 個項目帶有可攜性警告"},
    // src/core/engine/verify_engine.cpp
    {"cannot list volumes: {}", "無法列出磁碟區：{}", Match::Cause},
    {"archive {} is not on any attached volume", "封存 {} 不在任何已連接的磁碟區上"},
    {"run {} is not an interrupted run", "執行 {} 不是中斷的執行"},
    {"run {} has an invalid resume record", "執行 {} 的繼續紀錄無效"},
    {"media copy {} differs from the repository copy", "媒體副本 {} 與儲存庫中的副本不同"},
    {"the media holds revision {}, newer than the repository's {}: "
        "the repository may have been rolled back",
        "媒體上有版次 {}，比儲存庫的版次 {} 新：儲存庫可能已被回復到較舊的狀態"},
    {"no registered archive {}", "找不到已註冊的封存 {}"},
    {"UNKNOWN: this is not a registered archive; use `register` to create one",
        "未知：這不是已註冊的封存；請使用 `register` 建立封存"},
    {"AMBIGUOUS: the media matches several archives (cloned media); "
        "select one with `verify --full <archive_uuid>`",
        "無法確定：此媒體符合多個封存（複製的媒體）；"
        "請以 `verify --full <archive_uuid>` 選擇其中一個"},
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
    {"invalid recorded manifest header for {}: {}", "封存 {} 記錄的清單檔標頭無效：{}",
        Match::Cause},
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
    {"cannot scan the new root: {}", "無法掃描新根目錄：{}", Match::Cause},
    {"the new carrier does not hold the recorded content; "
        "nothing was changed (use `register --new-revision` to re-baseline changed content)",
        "新載體上的內容與記錄不符；未做任何變更（若內容已變更，"
        "請使用 `register --new-revision` 重新建立基準）"},
    {"cannot record the relocation: {}", "無法記錄搬移：{}", Match::Cause},
    // src/core/engine/transfer.cpp
    {"cannot compare with the stored manifest", "無法與已儲存的清單檔比較"},
    {"revision {} is already known with different bytes", "版次 {} 已存在，但位元組內容不同"},
    {"revision {} is older than the known revision {}; "
        "pass --force-rollback to import it anyway",
        "版次 {} 比已知的版次 {} 舊；若仍要匯入，請加上 --force-rollback"},
    // src/core/engine/recorded_manifest.cpp
    {"revision {} is not recorded", "版次 {} 沒有紀錄"},
    {"the manifest differs from the repository record", "清單檔與儲存庫的紀錄不符"},
    {"revision {} is older than the known revision {}", "版次 {} 比已知的版次 {} 舊"},
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
