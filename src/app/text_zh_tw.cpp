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

    // Run-level failure classes in plain words (app/plain_words.h); --json
    // keeps the classes.
    {"the media is not the one recorded", "媒體不是記錄中的那一個"},
    {"the media matches more than one archive", "媒體符合多個封存"},
    {"the manifest's signature does not verify", "清單檔的簽章驗證失敗"},
    {"the manifest is not the one recorded", "清單檔不是記錄中的那一個"},
    {"a revision older than one already known", "版次比已知的版次舊"},
    {"the record of changes on the volume was interrupted", "磁碟區的變更紀錄中斷了"},
    {"files could not be read", "有檔案無法讀取"},
    {"the repository could not be read or written", "無法讀寫儲存庫"},

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
        "       veriarchive-cli --version\n"
        "\n"
        "  repo init | info | rebuild-index | verify-history\n"
        "  media list\n"
        "  register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision]\n"
        "           [--force-new-archive] [--copy-of UUID] [--write-media-copy]\n"
        "           [--resume [--confirm]] [--lost-manifest UUID [--confirm]]\n"
        "  verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd]\n"
        "         [--resume RUN_ID] [--add-chunk-hashes]\n"
        "  archive list | show <uuid> | retire <uuid> [--confirm]\n"
        "  archive relocate <uuid> <root_path> [--resume]\n"
        "  archive compare <uuid> <uuid>\n"
        "  archive abandon-registration <uuid> [--confirm] | abandon-relocation <uuid> [--confirm]\n"
        "  archive abandon-verification <uuid> [--confirm]\n"
        "  history <uuid>\n"
        "  export-sha256sums <uuid> --out FILE\n"
        "  export <uuid> --out DIR | import <file.vam> [--force-rollback]\n",
        "用法：veriarchive-cli [--config P] [--repository P] [--lang en|zh-TW] [--json] <command>\n"
        "      veriarchive-cli --version\n"
        "\n"
        "  repo init | info | rebuild-index | verify-history\n"
        "  media list\n"
        "  register <root_path> [--label ARCH-NNNN] [--io hdd|ssd] [--new-revision]\n"
        "           [--force-new-archive] [--copy-of UUID] [--write-media-copy]\n"
        "           [--resume [--confirm]] [--lost-manifest UUID [--confirm]]\n"
        "  verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd]\n"
        "         [--resume RUN_ID] [--add-chunk-hashes]\n"
        "  archive list | show <uuid> | retire <uuid> [--confirm]\n"
        "  archive relocate <uuid> <root_path> [--resume]\n"
        "  archive compare <uuid> <uuid>\n"
        "  archive abandon-registration <uuid> [--confirm] | abandon-relocation <uuid> [--confirm]\n"
        "  archive abandon-verification <uuid> [--confirm]\n"
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
    // A command that is not one, or none (decision 0017): also as JSON with --json.
    {"unknown command {}", "未知的指令 {}"},
    {"a command is needed; veriarchive-cli without arguments shows the commands",
        "需要指定指令；不加任何引數執行 veriarchive-cli 即可列出所有指令"},
    {"scanning: {}", "掃描中：{}", Match::FormatOnly},
    {"hashing: {}", "計算雜湊中：{}", Match::FormatOnly},
    {"comparing: {}/{} entries", "比較中：{}/{} 個項目"},
    {"comparing: {}/{} entry", "比較中：{}/{} 個項目"},
    // repo rebuild-index, import and export (R-94): their progress, and a cancel.
    {"reading manifests: {}/{}", "讀取清單檔中：{}/{}"},
    {"reading the manifest: {}/{} entries", "讀取清單檔中：{}/{} 個項目"},
    {"reading the manifest: {}/{} entry", "讀取清單檔中：{}/{} 個項目"},
    {"copying the manifest: {}", "複製清單檔中：{}", Match::FormatOnly},
    {"the rebuild of the index was cancelled; the index was left as it was", "重建索引已取消；索引維持原狀"},
    {"the import was cancelled; nothing was imported", "匯入已取消；未匯入任何內容"},
    {"the export was cancelled; nothing was written", "匯出已取消；未寫入任何內容"},
    // src/cli/repo_commands.cpp
    {"RepositoryRoot: {}", "儲存庫根目錄：{}"},
    {"repository_uuid: {}", "repository_uuid：{}"},
    {"repo_selected_by: {}", "儲存庫選定方式：{}"},
    {"config_file: {}", "設定檔：{}"},
    // This program's version (decision 0037): repo info, --version, Settings.
    {"program version: {}", "程式版本：{}"},
    {"VeriArchive version {}", "VeriArchive 版本 {}"},
    {"Program version: {}", "程式版本：{}"},
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
    {"History intact: {} verification run, hash chain verified.",
        "歷程完整：共 {} 次驗證執行，雜湊鏈已驗證。"},
    {"History BROKEN at run {}: {}", "歷程於執行 {} 處斷裂：{}"},
    {"previous row was deleted or edited (prev_hash does not link)",
        "前一筆紀錄已被刪除或修改（prev_hash 無法銜接）"},
    {"row was edited (row_hash does not match its contents)",
        "此筆紀錄已被修改（row_hash 與其內容不符）"},
    {"Rebuilt the index from manifests: {}, {}, {}.", "已從清單檔重建索引：{}、{}、{}。", Match::FormatOnly},
    {"unfinished registrations whose saved progress was kept: {}", "保留了已儲存進度的未完成註冊：{} 個"},
    // Each revision whose manifest the rebuild could not read, by what became
    // of its record (decision 0028): the archive, then what is wrong.
    {"kept as recorded: {} ({}): {}", "保留原紀錄：{}（{}）：{}", Match::Cause},
    {"left out of the index: {} ({}): {}", "已從索引移除：{}（{}）：{}", Match::Cause},
    {"not in the index: {} ({}): {}", "不在索引中：{}（{}）：{}", Match::Cause},
    {"skipped: {}", "已略過：{}"},
    // A rebuild that made the repository's database again (decision 0036):
    // what was wrong with it, where it was put aside, and what is lost.
    {"The repository's database was missing; a new one was made.", "儲存庫的資料庫已遺失；已建立新的資料庫。"},
    {"The repository's database was missing; the files left beside it were put aside as {} and a new one was made.",
        "儲存庫的資料庫已遺失；留在它旁邊的檔案已改名為 {} 留在一旁，並建立新的資料庫。", Match::FormatOnly},
    {"The repository's database was empty; it was put aside as {} and a new one was made.",
        "儲存庫的資料庫是空的；已將它改名為 {} 留在一旁，並建立新的資料庫。", Match::FormatOnly},
    {"The repository's database was not a database; it was put aside as {} and a new one was made.",
        "儲存庫的資料庫不是資料庫檔案；已將它改名為 {} 留在一旁，並建立新的資料庫。", Match::FormatOnly},
    {"The repository's database was damaged; it was put aside as {} and a new one was made.",
        "儲存庫的資料庫已損毀；已將它改名為 {} 留在一旁，並建立新的資料庫。", Match::FormatOnly},
    {"The verification history was kept only in the repository's database, so the manifests cannot give it back: "
        "every archive is listed as never verified until it is verified again, an archive that was retired is listed "
        "as registered, and unfinished registrations and relocations are gone. If a backup of the repository holds "
        "veriarchive.db, put that file in place of the new one and run `repo rebuild-index` again.",
        "驗證歷程只存在儲存庫的資料庫中，清單檔無法還原它：每個封存在再次驗證之前都會列為從未驗證，已停用的封存會列為"
        "已註冊，未完成的註冊與搬移也都不在了。若儲存庫的備份中有 veriarchive.db，請用那個檔案取代新的資料庫，"
        "再執行一次 `repo rebuild-index`。"},
    // A rebuild that stopped after it made the database again: the new one is
    // not filled, and every command refuses it until a rebuild finishes.
    {"the rebuild of the index was cancelled before it filled the new database; no command can use the repository "
        "until `repo rebuild-index` runs to its end",
        "重建索引在填好新的資料庫之前就已取消；在 `repo rebuild-index` 完整執行完畢之前，任何指令都無法使用這個儲存庫"},
    {"the rebuild of the index did not fill the new database, and no command can use the repository until `repo "
        "rebuild-index` runs to its end: {}",
        "重建索引沒有填好新的資料庫；在 `repo rebuild-index` 完整執行完畢之前，任何指令都無法使用這個儲存庫：{}",
        Match::Cause},
    {"usage: repo init | info | rebuild-index | verify-history",
        "用法：repo init | info | rebuild-index | verify-history"},
    // src/cli/media_commands.cpp
    {"read-only", "唯讀"},
    {"filesystem_uuid: {}  short: {}", "filesystem_uuid：{}  短格式：{}"},
    {"device: {}  serial: {}  bus: {}", "裝置：{}  序號：{}  匯流排：{}"},
    {"(none)", "（無）"},
    {"health: {}", "健康狀態：{}"},
    {"usage: media list", "用法：media list"},
    // src/app/device_alerts.cpp: what a device reports of its health (decision 0044).
    {"healthy", "健康"},
    {"warning", "警告"},
    {"unhealthy", "不健康"},
    {"cannot be read", "無法讀取"},
    {"predicting failure", "預測即將故障"},
    {"hardware error", "硬體錯誤"},
    {"not usable", "無法使用"},
    {"transient error", "暫時性錯誤"},
    {"being repaired or rebuilt", "修復或重建中"},
    {"missing", "找不到"},
    {"bad sectors", "有損壞的磁區"},
    {"SMART attributes past their threshold", "SMART 屬性超過門檻"},
    {"spare capacity low", "備用容量不足"},
    {"temperature too high", "溫度過高"},
    {"reliability degraded", "可靠度下降"},
    {"turned read-only", "已轉為唯讀"},
    {"volatile memory backup failed", "揮發性記憶體備援失效"},
    {"{} (serial {})", "{}（序號 {}）", Match::FormatOnly},
    {"{} (WWN {})", "{}（WWN {}）", Match::FormatOnly},
    {"{}: {}", "{}：{}", Match::FormatOnly},
    {"Device alert: {} reports its health as {}. Copy the archives on it to another device soon, and verify the "
     "copies.",
        "裝置警訊：{} 回報的健康狀態為「{}」。請盡快把其中的封存複製到另一個裝置，並驗證這些副本。", Match::FormatOnly},
    {"device health: {}", "裝置健康狀態：{}", Match::FormatOnly},
    {"Device health: {}", "裝置健康狀態：{}", Match::FormatOnly},
    // src/app/device_alerts.cpp: how fast a FULL verification read (decision 0044).
    // The marks of a run's device alerts (Archives, history).
    {"device reports a problem", "裝置回報異常"},
    {"reading slower", "讀取變慢"},
    {"read speed: {}", "讀取速度：{}", Match::FormatOnly},
    {"Read speed: {}", "讀取速度：{}", Match::FormatOnly},
    {"{}; the last FULL verification of this archive on this device, which finished {}, read {}",
        "{}；此封存上次在此裝置上、於 {} 完成的完整驗證為{}", Match::FormatOnly},
    {"Device alert: reading was {}% slower than the last FULL verification of this archive on this device ({} "
     "against {}, {})",
        "裝置警訊：讀取速度比此封存上次在此裝置上的完整驗證慢了 {}%（這次{}，上次{}，{}）", Match::FormatOnly},
    {"Device alert: parts of the content read markedly slower than in the last FULL verification of this archive on "
     "this device ({})",
        "裝置警訊：部分內容的讀取速度明顯比此封存上次在此裝置上的完整驗證慢（{}）", Match::FormatOnly},
    {"; from {}% to {}% of the content, {}% slower", "；內容的 {}% 至 {}% 處慢了 {}%", Match::FormatOnly},
    {"; and {} more such stretches", "；另有 {} 處這樣變慢", Match::FormatOnly},
    {". A device that reads markedly slower than before may be failing; another program reading it at the same time, "
     "or another device read through the same port or hub, also makes reading slower.",
        "。讀取明顯比以前慢的裝置可能即將故障；同時有其他程式在讀取它，或接在同一個連接埠或集線器上的另一個裝置也在被"
        "讀取，也會讓讀取變慢。"},
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
        "comparing it with revision {}, whose manifest is lost: damage since revision {} was recorded would not be "
        "found. Do this only where no backup of the repository, export of revision {} or media copy holds that "
        "manifest. The archive keeps its verification history and its hash chain, and never goes back to an earlier "
        "revision.",
        "從此根目錄註冊版次 {0}，會把它目前的內容當作封存新的基準，而不與清單檔已無法取回的版次 {1} 比對："
        "自版次 {2} 記錄以來發生的損壞將不會被發現。只有在儲存庫的備份、版次 {3} 的匯出檔和媒體副本中都沒有"
        "該清單檔時，才這麼做。封存會保留它的驗證歷程及其雜湊鏈，也絕不會回到較早的版次。",
        Match::FormatOnly},
    {"run the command again with --confirm to register revision {}", "若要註冊版次 {}，請加上 --confirm 再執行一次"},
    {"revision {}, whose manifest was lost, was left out of the index; its verifications stay in the history",
        "清單檔已無法取回的版次 {} 已從索引移除；它的驗證紀錄仍保留在歷程中", Match::FormatOnly},
    // A resume of a registration that an older version left, before it is
    // confirmed (decision 0022, point 7): "Unfinished registration of ...",
    // "root: {}" and "started: {}" are abandon-registration's, and the warning
    // is the question of Resume under Unfinished operations.
    {"run the command again with --confirm to resume it", "若要繼續，請加上 --confirm 再執行一次"},
    {"--io takes hdd or ssd", "--io 只接受 hdd 或 ssd"},
    {"not a directory: {}", "不是資料夾：{}"},
    // A root that is not there (decision 0029, point 7), named as typed.
    {"the folder or volume {} was not found", "找不到資料夾或磁碟區 {}"},
    {"the folder or volume {} was not found: no volume is attached as {}; check that the device that holds it is "
        "attached",
        "找不到資料夾或磁碟區 {}：目前沒有磁碟區連接為 {}；請確認存放它的裝置已連接"},
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
    {"coverage: {} of {} registered byte read and matched",
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
        "[--resume RUN_ID] [--add-chunk-hashes]",
        "用法：verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd] "
        "[--resume RUN_ID] [--add-chunk-hashes]"},
    // Adding chunk hashes (decision 0033): the revision that records them, or
    // why there is none (an engine sentence). The Merkle root is 總碼 here.
    {"chunk hashes added: revision {} records them (chunk size {}, {} chunks, Merkle root {})",
        "已加入區塊雜湊：由版次 {} 記錄（區塊大小 {}，{} 個區塊，總碼 {}）"},
    {"chunk hashes not added: {}", "未加入區塊雜湊：{}", Match::Cause},
    // A run that went on past a changed enclosure (decision 0047): the
    // revision that records the new enclosure, or why there is none (an
    // engine sentence).
    {"new enclosure recorded: revision {} records it ({})", "已記下新的外接盒：由版次 {} 記錄（{}）"},
    {"new enclosure not recorded: {}", "未記下新的外接盒：{}", Match::Cause},
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
    {"content: {}, {}, {}, {}", "內容：{}、{}、{}、{}", Match::FormatOnly},
    // A count in the singular for one, the plural otherwise (app/plain_words.h).
    {"{} file", "{} 個檔案", Match::FormatOnly},
    {"{} empty directory", "{} 個空資料夾", Match::FormatOnly},
    {"{} empty directories", "{} 個空資料夾", Match::FormatOnly},
    {"{} link", "{} 個連結", Match::FormatOnly},
    {"{} links", "{} 個連結", Match::FormatOnly},
    {"{} byte", "{} 位元組", Match::FormatOnly},
    {"{} bytes", "{} 位元組", Match::FormatOnly},
    {"{} entry", "{} 個項目", Match::FormatOnly},
    {"{} entries", "{} 個項目", Match::FormatOnly},
    {"{} revision", "{} 個版次", Match::FormatOnly},
    {"{} revisions", "{} 個版次", Match::FormatOnly},
    {"{} day", "{} 天", Match::FormatOnly},
    {"{} days", "{} 天", Match::FormatOnly},
    {"copy of: {}", "副本來源：{}"},
    {"revisions:", "版次："},
    // An archive whose latest manifest cannot be used (decision 0028), and a
    // retired archive whose latest manifest is lost (point 9), in the
    // glossary's term (a lost manifest: 無法取回的清單檔).
    {"problem: {}", "問題：{}", Match::Cause},
    {"manifest lost: {}", "清單檔無法取回：{}", Match::Cause},
    // Each revision's chunks (decision 0032); the Merkle root is 總碼 in the interface.
    {"chunk size 0: whole-file hashes only", "區塊大小 0：只有整個檔案的雜湊"},
    {"chunk size {}, {} chunks, Merkle root {}", "區塊大小 {}，{} 個區塊，總碼 {}"},
    // The archive's unfinished registration and relocation in archive list and show (decision 0026), from
    // src/app/unfinished_operations.cpp; the progress and state are the words of Unfinished operations.
    {"Unfinished registration of revision {} from \"{}\": {}; {}", "未完成的註冊（版次 {}，根目錄「{}」）：{}；{}",
        Match::FormatOnly},
    {"Unfinished relocation of revision {} to \"{}\": {}; {}", "未完成的搬移（版次 {}，至「{}」）：{}；{}",
        Match::FormatOnly},
    {"Unfinished verification of revision {} adding chunk hashes, from \"{}\": {}; {}",
        "未完成的驗證（加入區塊雜湊，版次 {}，根目錄「{}」）：{}；{}", Match::FormatOnly},
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
    {"{}  {} VERIFY {}  r{}  {}", "{}  {} {}  r{}  {}", Match::FormatOnly},
    // The runs of a relocation in history and the window's History (decision 0026), from
    // src/app/history.cpp.
    {"interrupted relocation attempt", "中斷的搬移嘗試"},
    {"relocation from r{}", "從 r{} 搬移", Match::FormatOnly},
    // A run that went on past a changed enclosure (decision 0047): how it was
    // confirmed, beside it in history and the window's History.
    {"enclosure changed, same device by its serial", "外接盒已更換，裝置本身的序號相符"},
    {"enclosure changed, confirmed by the operator after reading the device's serial", "外接盒已更換，讀取序號後由您確認"},
    {"enclosure changed, confirmed by the operator without the serial", "外接盒已更換，由您確認、未讀序號"},
    {"several archives start with {}", "有多個封存以 {} 開頭"},
    {"no archive {}", "找不到封存 {}"},
    {"usage: archive list | show <uuid> | retire <uuid> [--confirm] | relocate <uuid> <root_path> "
        "[--io hdd|ssd] [--resume] | compare <uuid> <uuid> | abandon-registration <uuid> [--confirm] | "
        "abandon-relocation <uuid> [--confirm] | abandon-verification <uuid> [--confirm]",
        "用法：archive list | show <uuid> | retire <uuid> [--confirm] | relocate <uuid> <root_path> "
        "[--io hdd|ssd] [--resume] | compare <uuid> <uuid> | abandon-registration <uuid> [--confirm] | "
        "abandon-relocation <uuid> [--confirm] | abandon-verification <uuid> [--confirm]"},
    // archive compare: A is the reference archive, B the copy compared with it.
    {"COMPARE {}  {} ({}) r{}  vs  {} ({}) r{}", "比較結果：{}  {}（{}）r{}  與  {}（{}）r{}"},
    {"A: {}, revision {} recorded {}, {}", "A：{}，版次 {} 記錄於 {}，共 {}", Match::FormatOnly},
    {"B: {}, revision {} recorded {}, {}", "B：{}，版次 {} 記錄於 {}，共 {}", Match::FormatOnly},
    {"same: {}", "相同：{}"},
    {"This compares what each registration read. A FULL verify of each archive proves that its "
        "carrier still holds that content.",
        "此比較依據各封存註冊時讀取的內容。對各封存進行完整驗證，才能證明其載體仍保有這些內容。"},
    {"comparing: reading the manifests", "比較中：正在讀取清單檔"},
    // archive abandon-registration (decision 0025)
    {"Unfinished registration of {} ({}), revision {}", "{}（{}）未完成的註冊，版次 {}"},
    {"started: {}", "開始時間：{}"},
    {"saved progress: {}, {}", "已儲存的進度：{}，{}", Match::FormatOnly},
    {"Abandoning it removes its saved progress, and it can never be resumed afterwards. "
        "Recorded revisions, manifests and the verification history stay as they are.",
        "放棄後，已儲存的進度會被刪除，而且之後再也無法繼續。已記錄的版次、清單檔與驗證歷程都維持不變。"},
    {"The archive has no revision yet: it is removed with its registration.",
        "此封存還沒有任何版次：會隨著它的註冊一起移除。"},
    {"run the command again with --confirm to abandon it", "若要放棄，請加上 --confirm 再執行一次"},
    {"Abandoned the unfinished registration of {} ({}), revision {}: its saved progress ({}, {}) was removed.",
        "已放棄 {}（{}）未完成的註冊，版次 {}：已刪除其已儲存的進度（{}，{}）。"},
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
    {"Abandoned the unfinished relocation of {} ({}): its saved progress ({}, {}) was removed.",
        "已放棄 {}（{}）未完成的搬移：已刪除其已儲存的進度（{}，{}）。"},
    // archive abandon-verification (decision 0033); the other lines are
    // abandon-registration's and abandon-relocation's.
    {"Unfinished verification of {} ({}) that adds chunk hashes, revision {}",
        "{}（{}）加入區塊雜湊的未完成驗證，版次 {}"},
    {"Abandoned the unfinished verification of {} ({}) that adds chunk hashes: its saved progress ({}, {}) was "
        "removed.",
        "已放棄 {}（{}）加入區塊雜湊的未完成驗證：已刪除其已儲存的進度（{}，{}）。"},
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
    {"{} of {} file ({}%)", "{} / {} 個檔案（{}%）", Match::FormatOnly},
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
    // src/app/keep_awake.h: why the computer is kept awake, as the system
    // lists it (decision 0043).
    {"A long operation is running", "長時間作業執行中"},

    // Graphical interface
    // src/gui/window.cpp: the sidebar's pages and the header above it
    // (decision 0046); Register Archive is the button above the sidebar, and
    // the title of its dialog; Compare Archives the comparison dialog's.
    {"Archives", "封存"},
    {"Devices", "裝置"},
    {"History", "歷程"},
    {"Register Archive", "註冊封存"},
    {"Compare Archives", "比較封存"},
    {"Settings", "設定"},
    // src/gui/gui_app.cpp
    // The window's notice while the open upgrades the repository database,
    // in place of the views (decision 0032).
    {"Upgrading the repository database...", "正在升級儲存庫資料庫……"},
    {"VeriArchive first keeps a copy of the database from before the upgrade, beside it. For a large repository "
        "this takes a while; the views open when it is done.",
        "VeriArchive 會先在資料庫旁保留一份升級前的副本。儲存庫較大時，這需要一些時間；完成後就會顯示各頁。"},
    // src/gui/gui_support.cpp
    {"Scanning: {}", "掃描中：{}", Match::FormatOnly},
    {"Hashing: {}", "計算雜湊中：{}", Match::FormatOnly},
    // src/gui/progress_wording.cpp: an export's bar, the manifest's bytes copied (R-94).
    {"Copying: {}", "複製中：{}", Match::FormatOnly},
    {"Comparing: {} / {} entries", "比較中：{} / {} 個項目"},
    {"Comparing: {} / {} entry", "比較中：{} / {} 個項目"},
    {"Starting...", "正在啟動……"},
    {"Unexpected error: {}", "未預期的錯誤：{}"},
    // src/gui/archives_page.cpp, src/gui/archive_rows.cpp: Archives
    // (decision 0046), its list of archives and what it says when there are
    // none.
    {"Problem: {}", "問題：{}", Match::FormatOnly},  // an archive whose manifest cannot be used (decision 0028)
    // A retired archive whose latest manifest is lost (decision 0028, point 9).
    {"Manifest lost: {}", "清單檔無法取回：{}", Match::FormatOnly},
    {"No repository is open; see Settings.", "目前沒有開啟的儲存庫；請參閱「設定」。"},
    {"{} archive", "{} 個封存", Match::FormatOnly},
    {"{} archives", "{} 個封存", Match::FormatOnly},
    {"Refresh", "重新整理"},
    {"Last FULL verify {}", "上次完整驗證 {}", Match::FormatOnly},
    {"Never verified", "從未驗證"},
    {"No archives yet", "還沒有封存"},
    {"Register a folder or volume: VeriArchive records the fingerprints of each of its files. A FULL verification "
     "later tells whether any of them was modified, corrupted, deleted or added.",
        "註冊一個資料夾或磁碟區：VeriArchive 會記下其中每個檔案的指紋。之後進行完整驗證，就能查出是否有檔案被修改、"
        "損毀、刪除，或多了檔案。"},
    {"Register the first archive", "註冊第一個封存"},
    // How long ago a verification ended, and how long a run took.
    {"less than an hour ago", "不到 1 小時前"},
    {"1 hour ago", "1 小時前"},
    {"{} hours ago", "{} 小時前", Match::FormatOnly},
    {"1 day ago", "1 天前"},
    {"{} days ago", "{} 天前", Match::FormatOnly},
    {"{} weeks ago", "{} 週前", Match::FormatOnly},
    {"{} months ago", "{} 個月前", Match::FormatOnly},
    {"1 year ago", "1 年前"},
    {"{} years ago", "{} 年前", Match::FormatOnly},
    {"1 year 1 month ago", "1 年 1 個月前"},
    {"1 year {} months ago", "1 年 {} 個月前", Match::FormatOnly},
    {"{} years 1 month ago", "{} 年 1 個月前", Match::FormatOnly},
    {"{} years {} months ago", "{} 年 {} 個月前", Match::FormatOnly},
    {"{} s", "{} 秒", Match::FormatOnly},
    {"{} min {} s", "{} 分 {} 秒", Match::FormatOnly},
    {"{} h {} min", "{} 小時 {} 分", Match::FormatOnly},
    // src/gui/archive_detail.cpp, src/gui/archive_rows.cpp: the details of
    // an archive (decision 0046): its menu, the banner of its last FULL
    // verification (R-21, R-105), FULL verify and its menu (decision 0033),
    // and its groups.
    {"Menu of {}", "{} 的選單", Match::FormatOnly},
    {"Compare with another archive...", "與另一個封存比較…"},
    {"Compares the manifests in the repository; no media is read.", "比較儲存庫中的清單檔；不讀取媒體。"},
    {"The repository has no other archive to compare with.", "儲存庫中沒有可以比較的其他封存。"},
    {"Create a new revision...", "建立新版次…"},
    {"After an intended change of its content: reads the whole archive again and takes what it holds now as the "
     "new baseline.",
        "內容有意更改之後：重新讀取整個封存，以它現在的內容作為新的基準。"},
    {"The archive's root is not known: choose its folder or volume in Register Archive.",
        "不知道此封存的根目錄：請在「註冊封存」選擇它的資料夾或磁碟區。"},
    {"A retired archive takes no new revisions.", "已停用的封存不再接受新的版次。"},
    {"Copy archive UUID", "複製封存 UUID"},
    {"Archive UUID copied", "已複製封存 UUID"},
    {"Last FULL verify: {}, {}", "上次完整驗證：{}，{}", Match::FormatOnly},
    {"Last verify: {}, {}, {}", "上次驗證：{}，{}，{}", Match::FormatOnly},
    {"No FULL verification yet. A FULL verification tells whether the media still holds the content that the "
     "archive's current revision recorded.",
        "還沒有完整驗證過。完整驗證一次，就能查出媒體是否仍保有目前版次所記錄的內容。"},
    {"OVERDUE: no FULL verify for more than 1 day", "逾期：超過 1 天沒有完整驗證"},
    {"OVERDUE: no FULL verify for more than {} days", "逾期：超過 {} 天沒有完整驗證", Match::FormatOnly},
    {"OVERDUE: no verify of any mode for more than 1 day", "逾期：超過 1 天沒有任何模式的驗證"},
    {"OVERDUE: no verify of any mode for more than {} days", "逾期：超過 {} 天沒有任何模式的驗證", Match::FormatOnly},
    {"Retired: this archive is no longer expected to be verified, and takes no new revisions.",
        "已停用：這個封存不必再驗證，也不再接受新的版次。"},
    {"FULL verify", "完整驗證"},
    {"Other ways to verify", "其他驗證方式"},
    {"Every file is read once; when every entry matches, the next revision records the same entries with their "
     "chunk hashes. Any difference records the verification alone.",
        "每個檔案只讀取一次；所有項目都相符時，下一個版次會記錄相同的項目及其區塊雜湊。有任何不同時，只記錄這次驗證的"
        "結果。"},
    {"Cannot tell whether chunk hashes can be added: {}", "無法判斷能否加入區塊雜湊：{}", Match::FormatOnly},
    {"A verification of this archive that adds chunk hashes is unfinished: resume it, or abandon it under "
     "Unfinished operations.",
        "此封存有一個加入區塊雜湊的未完成驗證：請繼續它，或在「未完成的作業」中放棄它。"},
    {"FULL verify with strict metadata", "以嚴格中繼資料完整驗證"},
    {"Timestamp and attribute differences fail the run", "時間戳記與屬性差異會使執行失敗"},
    {"Verify a folder or volume...", "驗證一個資料夾或磁碟區…"},
    {"Verifies the archive on the folder or volume you choose, for when more than one attached volume holds this "
     "archive",
        "驗證您選擇的資料夾或磁碟區上的封存；用於有多個已連接的磁碟區都存放此封存時"},
    // Its dialog: the folder or volume typed, pasted or chosen, and strict
    // metadata ("Strict metadata" is Settings').
    {"Verify a folder or volume", "驗證一個資料夾或磁碟區"},
    {"Folder or volume", "資料夾或磁碟區"},
    // No second verification while the archive's job waits or works (decision 0045).
    {"{} already has a job that waits or works: stop it in Jobs first, or wait for it to end.",
        "{} 已經有一個工作在等候或執行；請先在「工作」停止它，或等它結束。", Match::FormatOnly},
    {"The last FULL verification of this archive stopped at {}. Resume continues it without reading again what it "
     "read.",
        "此封存上次的完整驗證停止於 {}。按「繼續」即可接續，已讀過的不必重讀。", Match::FormatOnly},
    {"The last FULL verification of this archive that adds chunk hashes stopped at {}. Resume continues it without "
     "reading again what it read.",
        "此封存上次加入區塊雜湊的完整驗證停止於 {}。按「繼續」即可接續，已讀過的不必重讀。", Match::FormatOnly},
    {"Device", "裝置"},
    {"Volume", "磁碟區"},
    {"Capacity", "容量"},
    {"Model", "型號"},
    {"Bus", "匯流排"},
    {"Serial", "序號"},
    {"WWN", "WWN"},
    {"Health in the last verification", "上次驗證時的健康狀態"},
    {"Read speed of the last FULL verify", "上次完整驗證的讀取速度"},
    {"Content", "內容"},
    {"Files", "檔案"},
    {"{}, recorded {}", "{}，記錄於 {}", Match::FormatOnly},
    {"Chunk hashes", "區塊雜湊"},
    {"yes (chunk size {})", "有（區塊大小 {}）", Match::FormatOnly},
    {"none", "沒有"},
    {"Archive UUID", "封存 UUID"},
    {"The next FULL verification can add chunk hashes (in the menu beside FULL verify), so that a verification can "
     "tell which part of a large file differs.",
        "下一次完整驗證時可以一併加入區塊雜湊（在「完整驗證」旁的選單中），讓驗證能指出大檔案是哪一個區塊不同。"},
    {"Copies", "副本"},
    {"This archive is recorded as a copy of it", "此封存記錄為它的副本"},
    {"Recorded as a copy of this archive", "記錄為此封存的副本"},
    {"not in this repository", "不在這個儲存庫中"},
    {"Compare...", "比較…"},
    {"Recent verifications", "最近的驗證"},
    {"See where it slowed", "查看變慢的位置"},
    {"Cannot read the verifications of this archive: {}", "無法讀取此封存的驗證：{}", Match::FormatOnly},
    {"Every verification of this archive in History ({})", "在「歷程」中查看此封存的全部驗證（{}）", Match::FormatOnly},
    // src/gui/archive_detail.cpp, src/gui/archive_actions.cpp: the rest of the
    // details' menu (decision 0046): the export of the archive's manifest, its
    // relocation to a new carrier and its retirement, where each applies.
    {"Export manifest...", "匯出清單檔…"},
    {"Relocate to a new carrier...", "搬移到新的載體…"},
    {"Retire archive...", "停用封存…"},
    {"The manifest of its latest revision cannot be used, and a relocation compares the new carrier with it.",
        "它最新版次的清單檔無法使用，而搬移需要拿新載體與它比對。"},
    {"After you copied the archive to another device, or put its device in another enclosure: reads the new carrier "
     "in full and records it as the archive's next revision.",
        "您把封存複製到另一個裝置，或把它的裝置換了外接盒之後：完整讀取新的載體，並記錄為封存的下一個版次。"},
    {"The manifest of its latest revision cannot be used: there is nothing to export.",
        "它最新版次的清單檔無法使用：沒有可以匯出的內容。"},
    {"Writes the manifest of revision {} and identity.json into a folder named for the archive inside the folder "
     "you choose; `import` on the command line takes it into this repository or another.",
        "在您選擇的資料夾中建立以封存命名的資料夾，寫入版次 {} 的清單檔與 identity.json；可在命令列以 `import` 匯入此"
        "儲存庫或另一個儲存庫。",
        Match::FormatOnly},
    // While the archive has an unfinished relocation, no second one begins
    // (decision 0026): the stopped one is resumed.
    {"Resumes the stopped relocation to {}, also at another root where the new carrier is attached at another drive "
     "letter or mount point now.",
        "繼續已停止、搬移至 {} 的作業；如果新的載體現在接在另一個磁碟機代號或掛載點，也可以改在那裡的根目錄繼續。",
        Match::FormatOnly},
    {"An unfinished relocation of this archive to {} is listed under Unfinished operations in Archives ({}).",
        "此封存有一個搬移至 {} 的未完成作業，列在「封存」頁的「未完成的作業」中（{}）。", Match::FormatOnly},
    {"This archive is retired already.", "這個封存已經停用。"},
    {"It is no longer expected to be verified and keeps its history. A retired archive cannot be made active again.",
        "它不必再驗證，並保留其歷程。已停用的封存無法再恢復使用。"},
    // src/gui/device_rows.cpp, src/gui/archives_page.cpp,
    // src/gui/archive_detail.cpp: where an archive is attached now, as the
    // latest read of the attached devices found it (decision 0046): the first
    // line of its row, the notice of its details, the tooltip of FULL verify,
    // and its rows in the group Device.
    {"Not attached", "未連接"},
    {"The device differs from the one recorded", "裝置與記錄中的不同"},
    {"Several volumes look like this archive", "多個磁碟區都像這個封存"},
    {"{}, serial {}", "{}，序號 {}", Match::FormatOnly},
    {"{}, WWN {}", "{}，WWN {}", Match::FormatOnly},
    {"The device of this archive ({}) is not attached now. Once it is attached, the archive can be verified.",
        "這個封存的裝置（{}）目前沒有連接。接上之後就能驗證。", Match::FormatOnly},
    {"The volume of this archive ({}) is not attached now. Once it is attached, the archive can be verified.",
        "這個封存的磁碟區（{}）目前沒有連接。接上之後就能驗證。", Match::FormatOnly},
    {"The volume of this archive is not attached now. Once it is attached, the archive can be verified.",
        "這個封存的磁碟區目前沒有連接。接上之後就能驗證。"},
    // MIGRATED: what it means; then, on a line of its own, how the archive is
    // relocated to the new carrier.
    {"The filesystem of this archive is attached at {}, but on another device than the one recorded (MIGRATED): the "
     "archive's device in another enclosure, or a sector-by-sector clone of it. FULL verify does not read it there; a "
     "clone is registered as a separate archive in Register Archive.",
        "這個封存的檔案系統出現在 {}，但裝置與記錄中的不同（已遷移）：可能是封存的裝置換了外接盒，也可能是逐磁區複製"
        "出來的複本。「完整驗證」不會讀取那裡；複本要在「註冊封存」中註冊為另一個封存。",
        Match::FormatOnly},
    // ... and how the archive is relocated to it in the window (decision 0046).
    {"If it is this archive's device in another enclosure, or you moved the archive to that device on purpose, "
     "relocate the archive to it with Relocate to this carrier..., which reads it in full first.",
        "如果那是這個封存的裝置換了外接盒，或是您有意把封存移到那個裝置，請按「搬移到這個載體…」把封存搬移過去；"
        "搬移前會先完整讀取它。"},
    {"If one of them is this archive's device in another enclosure, or you moved the archive to that device on "
     "purpose, relocate the archive to it with Relocate to a new carrier... in the menu of these details, which "
     "reads it in full first.",
        "如果其中之一是這個封存的裝置換了外接盒，或是您有意把封存移到那個裝置，請在這些詳細資料的選單中按「搬移到新的"
        "載體…」把封存搬移過去；搬移前會先完整讀取它。"},
    {"Relocate to this carrier...", "搬移到這個載體…"},
    // ... and a retired archive's, which is not relocated (decision 0003).
    {"This archive is retired and takes no new revisions, so it is not relocated.",
        "此封存已停用，不再接受新的版次，因此不會搬移。"},
    {"More than one attached volume looks like this archive ({}), as a device and a sector-by-sector clone of it "
     "would: a verification by its UUID cannot tell which one to read. Choose \"Verify a folder or volume...\" in "
     "the menu beside \"FULL verify\", then choose the root of the one to verify, one of those above.",
        "有多個已連接的磁碟區都像這個封存（{}），例如一個裝置和它逐磁區複製出來的複本：依封存 UUID 驗證無法判斷要讀取"
        "哪一個。請從「完整驗證」旁的選單選擇「驗證一個資料夾或磁碟區…」，再從上面列出的根目錄中選擇要驗證的那一個。",
        Match::FormatOnly},
    // The tooltip of FULL verify, which stays offered: the read may be old.
    {"This archive was not attached when the attached volumes were read last. FULL verify looks for it again.",
        "上次讀取已連接的磁碟區時，這個封存沒有連接。「完整驗證」會再找一次。"},
    {"The filesystem of this archive is on another device than the one recorded (MIGRATED): FULL verify does not "
     "read it there.",
        "這個封存的檔案系統在與記錄中不同的裝置上（已遷移）：「完整驗證」不會讀取那裡。"},
    {"More than one attached volume looks like this archive: choose the one to verify with \"Verify a folder or "
     "volume...\" in the menu.",
        "有多個已連接的磁碟區都像這個封存：請用選單中的「驗證一個資料夾或磁碟區…」選擇要驗證的那一個。"},
    // Beside the carrier as recorded in the group Device: where the archive
    // is now.
    {"Attached volume", "目前所在的磁碟區"},
    {"Attached at", "目前的根目錄"},
    {"not attached", "未連接"},
    {"not as recorded: the filesystem is on another device ({})", "與記錄中的不同：檔案系統在另一個裝置上（{}）",
        Match::FormatOnly},
    // src/gui/devices_page.cpp, src/gui/device_rows.cpp,
    // src/gui/devices_poll.cpp: Devices (decision 0046), the devices attached
    // now with their volumes and the archives on them.
    {"This page lists the devices attached to this computer now, the health each reports, the volumes on each, and "
     "the archives whose roots lie on those volumes. It reads them again every 30 seconds while this page or "
     "Archives is shown, and at once with Refresh. Reading them needs no administrator permission, and this page "
     "writes nothing to them.",
        "這裡列出目前連接到這部電腦的裝置、各裝置回報的健康狀態、裝置上的磁碟區，以及根目錄位於這些磁碟區上的封存。"
        "顯示這一頁或「封存」時，每 30 秒會重新讀取一次；按「重新整理」會立即讀取。讀取時不需要系統管理員權限，這一頁"
        "也不會寫入這些裝置。"},
    {"Reading the attached devices...", "正在讀取已連接的裝置……"},
    {"Still reading: a device may not be responding", "仍在讀取：可能有裝置沒有回應"},
    {"1 device attached", "已連接 1 個裝置"},
    {"{} devices attached", "已連接 {} 個裝置", Match::FormatOnly},
    {"1 volume attached", "已連接 1 個磁碟區"},
    {"{} volumes attached", "已連接 {} 個磁碟區", Match::FormatOnly},
    {"No device is attached.", "沒有已連接的裝置。"},
    {"Cannot tell which archives are on them: {}", "無法判斷這些磁碟區上有哪些封存：{}", Match::FormatOnly},
    {"not reported", "裝置沒有回報"},
    {"no mount point", "沒有掛載點"},
    {"Volumes whose device is not known", "裝置不明的磁碟區"},
    {"The filesystem belongs to {}, but the device differs from the one recorded (MIGRATED)",
        "檔案系統屬於 {}，但裝置與記錄中的不同（已遷移）", Match::FormatOnly},
    {"Register this volume", "註冊這個磁碟區"},
    {"Being registered: {}", "正在註冊 {}", Match::FormatOnly},
    // The head of Archives, while the attached devices cannot be read.
    {"Cannot read the attached devices; see Devices", "無法讀取已連接的裝置，詳情請看「裝置」"},
    // src/gui/archives_page.cpp, src/gui/unfinished_actions.cpp,
    // src/app/unfinished_operations.cpp: unfinished operations (decision 0022)
    {"Unfinished operations", "未完成的作業"},
    {"Registrations, verifications and relocations that have not finished, also those started from the "
        "command line. The numbers move each time an operation saves its progress: after every 1000 files, "
        "or once a minute when a file finishes; not while it scans.",
        "尚未完成的註冊、驗證與搬移，包括從命令列啟動的。每當作業儲存進度，數字就會更新：每 1000 個檔案一次，"
        "或在檔案完成時每分鐘一次；掃描期間不會更新。"},
    {"Root", "根目錄"},
    {"Registration", "註冊"},
    {"Verification", "驗證"},
    {"Verification adding chunk hashes", "驗證（加入區塊雜湊）"},
    {"Relocation", "搬移"},
    {"{} of about {} ({}%)", "{} / 約 {}（{}%）", Match::FormatOnly},
    {"{} done", "已完成 {}", Match::FormatOnly},
    {"total not known: {}", "總量不明：{}", Match::FormatOnly},
    {"{} of {} files", "{} / {} 個檔案", Match::FormatOnly},
    {"{} of {} file", "{} / {} 個檔案", Match::FormatOnly},
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
    {"Its saved progress will be lost: {} ({}) hashed so far.", "已儲存的進度會遺失：目前已計算雜湊的 {}（{}）。",
        Match::FormatOnly},
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
    {"Its saved progress will be lost: {} ({}) matched so far.", "已儲存的進度會遺失：目前已比對相符的 {}（{}）。",
        Match::FormatOnly},
    {"It cannot be resumed afterwards. The archive, its revisions, manifests and the verification history are "
        "not changed.",
        "之後再也無法繼續。封存本身、已記錄的版次、清單檔與驗證歷程都不會變更。"},
    {"Reading the relocation of {} ...", "正在讀取 {} 的搬移……"},
    {"Abandoning the relocation of {} ...", "正在放棄 {} 的搬移……"},
    {"Abandoned the unfinished relocation of {} ({}).", "已放棄 {}（{}）未完成的搬移。"},
    {"This relocation is no longer listed as unfinished.", "此搬移已不在未完成的作業中。"},
    // Abandoning a verification that adds chunk hashes (decision 0033); the
    // detail's last sentences are a relocation's.
    {"Abandon the unfinished verification of {} that adds chunk hashes?", "要放棄 {} 加入區塊雜湊的未完成驗證嗎？"},
    {"Reading the verification of {} ...", "正在讀取 {} 的驗證……"},
    {"Abandoning the verification of {} ...", "正在放棄 {} 的驗證……"},
    {"Abandoned the unfinished verification of {} ({}) that adds chunk hashes.",
        "已放棄 {}（{}）加入區塊雜湊的未完成驗證。"},
    {"Cannot read the unfinished operations: {}", "無法讀取未完成的作業：{}", Match::FormatOnly},
    // src/gui/history_page.cpp, src/gui/history_rows.cpp: History, the runs
    // of every archive or of the one its filter names (decision 0046), and
    // the check of the chain in the page.
    {"Archive", "封存"},
    {"All archives", "全部封存"},
    {"Check history chain", "檢查歷程鏈"},
    {"Cannot check the history: {}", "無法檢查歷程：{}"},
    // The chain protects the history of the whole repository, whatever the filter lists.
    {"The history of every archive is intact: {} verification runs, hash chain verified.",
        "全部封存的歷程完整：共 {} 次驗證執行，雜湊鏈已驗證。", Match::FormatOnly},
    {"The history of every archive is intact: {} verification run, hash chain verified.",
        "全部封存的歷程完整：共 {} 次驗證執行，雜湊鏈已驗證。", Match::FormatOnly},
    {"History BROKEN at run {} ({} · {}): {}", "歷程於執行 {}（{}・{}）處斷裂：{}", Match::FormatOnly},
    {"Cannot read the verification history: {}", "無法讀取驗證歷程：{}", Match::FormatOnly},
    {"Never verified.", "從未驗證。"},
    {"No verification is recorded yet.", "還沒有任何驗證紀錄。"},
    {"Stopped at {}%", "停止於 {}%", Match::FormatOnly},
    {"Show {} more runs (of {})", "再顯示 {} 次執行（還有 {} 次）", Match::FormatOnly},
    // src/gui/result_page.cpp, src/gui/run_result.cpp: the result page of a
    // run (decision 0046): its banner, its items, its metrics, its read speed
    // by position in the content (decision 0044), its device and identity
    // and its technical details.
    {"View result", "查看結果"},
    {"Cannot read this run: {}", "無法讀取這次執行：{}", Match::FormatOnly},
    {"This run is no longer recorded.", "已沒有這次執行的紀錄。"},
    {"100% of the registered content was read and matched", "100% 的已註冊內容都已讀取且相符"},
    {"Stopped at {}%: {} of {} read.", "停止於 {}%：已讀取 {} / {}。", Match::FormatOnly},
    {"Stopped before it reached a result; {} read and matched in this run.",
        "在得出結果之前就停止了；這次執行中已讀取且相符的內容共 {}。", Match::FormatOnly},
    {"A report cannot be saved for a verification that has not finished.", "未完成的驗證不能儲存報告。"},
    {"Items ({})", "項目（{}）", Match::FormatOnly},
    {"Show {} more items (of {})", "再顯示 {} 個項目（還有 {} 個）", Match::FormatOnly},
    {"Every entry was verified.", "每個項目都已驗證。"},
    {"No entry was recorded as differing.", "沒有記錄任何不符的項目。"},
    {"Read and matched", "已讀取且相符"},
    {"Entries compared", "已比對的項目"},
    {"Duration", "耗時"},
    {"Average read speed", "平均讀取速度"},
    {"Read speed by position in the content", "讀取速度（依內容位置）"},
    {"This verification", "這次"},
    {"The one compared with (dashed)", "上次（虛線）"},
    {"Markedly slower", "明顯變慢之處"},
    {"Vertical axis: MiB/s", "縱軸：每秒 MiB"},
    {"No earlier FULL verification of this archive read the same content on this device, so there is nothing to "
     "compare with.",
        "此封存先前沒有在此裝置上讀取相同內容的完整驗證，所以沒有可比較的對象。"},
    {"The version that ran this verification did not record read speed yet.", "這次驗證時的版本還不記錄讀取速度。"},
    {"This verification read too little to chart its read speed by position in the content.",
        "這次驗證讀取的內容太少，無法畫出依內容位置的讀取速度。"},
    {"A relocation does not record its read speed.", "搬移不記錄讀取速度。"},
    {"Device and identity", "裝置與身分"},
    {"Media identity", "媒體身分"},
    {"not read, as the manifest of this revision cannot be used", "未讀取，因為這個版次的清單檔無法使用"},
    {"Health", "健康狀態"},
    {"Technical details", "技術細節"},
    {"Run", "執行"},
    {"Mode", "模式"},
    {"Revision", "版次"},
    {"{}, which recorded the chunk hashes added to revision {}", "{}，記錄了為版次 {} 加入的區塊雜湊",
        Match::FormatOnly},
    {"{}, adding chunk hashes", "{}，加入區塊雜湊中", Match::FormatOnly},
    {"{}; no revision recorded the chunk hashes added", "{}；沒有版次記錄加入的區塊雜湊", Match::FormatOnly},
    {"Resumed from", "繼續自"},
    {"Note", "備註"},
    {"Started", "開始時間"},
    {"Finished", "完成時間"},
    {"Entries by status", "各狀態的項目數"},
    {"Bytes matched", "相符位元組"},
    {"Failures", "失敗類別"},
    {"Computer", "電腦"},
    {"Program version", "程式版本"},
    {"Guarantee", "保證範圍"},
    // src/gui/settings_page.cpp: its groups (decision 0046) and their rows.
    {"Repository", "儲存庫"},
    {"Interface", "介面"},
    {"Verification policy", "驗證政策"},
    {"About", "關於"},
    {"No repository is open.", "目前沒有開啟的儲存庫。"},
    {"On first use, initialise a repository: it is created next to the program (or in your "
        "home folder when that is not writable) and remembered in the config file.",
        "第一次使用時，請初始化儲存庫：它會建立在程式所在資料夾（若該處無法寫入，"
        "則建立在您的個人資料夾），並記錄在設定檔中。"},
    {"RepositoryRoot", "儲存庫根目錄"},
    {"Selected by", "選定方式"},
    {"repository_uuid", "repository_uuid"},
    {"Config file", "設定檔"},
    {"Overdue after", "逾期期限"},
    {"FULL verify {}, any verify {}", "完整驗證 {}，任何驗證 {}", Match::FormatOnly},
    {"Strict metadata", "嚴格中繼資料"},
    {"On", "開啟"},
    {"Off", "關閉"},
    {"Trusted signing keys", "受信任的金鑰"},
    {"Signed manifests are not part of this version.", "這個版本尚未提供簽章清單檔。"},
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
    {"Wait for the jobs to end, or stop them, before changing the language.",
        "請等工作結束或先停止它們，再切換語言。"},
    // src/gui/jobs_page.cpp, src/gui/job_words.cpp, src/gui/jobs.cpp: the
    // window's jobs (decision 0045), the groups of the Jobs page and the
    // header's jobs button with its popover (decision 0046).
    {"Jobs", "工作"},
    {"Running", "執行中"},
    {"Waiting", "等候中"},
    {"See all jobs", "查看全部工作"},
    {"Jobs: {} running, {} waiting", "工作：{} 個執行中、{} 個等候中", Match::FormatOnly},
    {"Jobs: {} running", "工作：{} 個執行中", Match::FormatOnly},
    {"Jobs: none running", "工作：沒有執行中的工作"},
    {"Each registration, verification, comparison, relocation and export started in this window runs as a job. "
     "Jobs that read the same device wait for one another; the others run at once. A stopped registration, "
     "verification or relocation keeps what it did: it can be resumed without reading again what it read.",
        "在這個視窗開始的每個註冊、驗證、比較、搬移和匯出，都會成為一個工作。讀取同一個裝置的工作會依序等候，其他的同時"
        "執行。停止的註冊、驗證或搬移會保留已完成的部分：之後可以繼續，已讀過的不必重讀。"},
    {"Clear ended jobs", "清除已結束的工作"},
    {"No jobs yet.", "還沒有工作。"},
    {"Stop", "停止"},
    {"Details", "詳細"},
    // What a screen reader calls a row's Stop and Details: with the job's title.
    {"Stop {}", "停止 {}", Match::FormatOnly},
    {"Details of {}", "{} 的詳細資料", Match::FormatOnly},
    {"Result of {}", "{} 的結果", Match::FormatOnly},
    {"Close", "關閉"},
    {"Job details", "工作詳細資料"},
    {"Ended", "已結束"},
    {"Ended: {}", "已結束：{}", Match::FormatOnly},
    {"Stopping...", "停止中……"},
    {"Waiting: {} reads the same device", "等候中：{} 正在讀取同一個裝置", Match::FormatOnly},
    {"Waiting for its turn", "等候中"},
    {"The report shows here once the job has ended.", "工作結束後，報告會顯示在這裡。"},
    {"Stopped before it started, while it waited for the device.", "在等候裝置時就停止了，尚未開始。"},
    {"Register {}", "註冊 {}", Match::FormatOnly},
    {"FULL verify {}", "完整驗證 {}", Match::FormatOnly},
    {"Resume the FULL verification of {}", "繼續完整驗證 {}", Match::FormatOnly},
    {"Compare {} with {}", "比較 {} 與 {}", Match::FormatOnly},
    {"1 job is still running.", "還有 1 個工作在執行。"},
    {"{} jobs are still running.", "還有 {} 個工作在執行。", Match::FormatOnly},
    {"Closing the window stops them. Each can be resumed later, without reading again what it read.",
        "關閉視窗會停止它們。之後每個都可以繼續，已讀過的不必重讀。"},
    {"Keep the window open", "不要關閉"},
    {"Stop them and close", "停止並關閉"},
    {"A job of this window is working on this operation. Stop it in Jobs first, or wait for it to end.",
        "這個視窗有一個工作正在處理這個作業。請先到「工作」停止它，或等它結束。"},
    // src/gui/register_wizard.cpp, src/gui/register_choices.cpp: Register
    // Archive (decision 0046), a dialog of three pages, and the report of its
    // job.
    {"{}, {}, {}, {} hashed", "{}、{}、{}，已計算雜湊 {}", Match::FormatOnly},
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
    {"Open a repository first (Settings).", "請先開啟儲存庫（見「設定」）。"},
    {"Cannot open the repository: {}", "無法開啟儲存庫：{}"},
    {"Not registered: {}", "未能註冊：{}"},
    {"Done", "完成"},
    {"Registration reads every file of the folder or volume and records its fingerprints "
        "(BLAKE3 and SHA-256) as the baseline that later verifications compare against. "
        "Nothing is written to the archive unless you ask for a media copy.",
        "註冊會讀取資料夾或磁碟區中的每個檔案，並記錄其指紋（BLAKE3 與 SHA-256）作為基準，"
        "供日後驗證比對。除非您要求寫入媒體副本，否則不會對封存寫入任何內容。"},
    {"Choose...", "選擇…"},
    {"Choose the archive folder or volume", "選擇封存資料夾或磁碟區"},
    {"Register as a separate archive even though this folder or volume is registered",
        "即使此資料夾或磁碟區已註冊，仍註冊為另一個封存"},
    {"Also write a manifest copy to the media (.veriarchive)",
        "同時將清單檔寫入媒體作為媒體副本（.veriarchive）"},
    {"Cancel", "取消"},
    // Its pages and their steps.
    {"{}. {}", "{}. {}", Match::FormatOnly},
    {"Choose the source", "選擇來源"},
    {"Options", "選項"},
    {"Confirm", "確認"},
    {"Back", "上一步"},
    {"Next", "下一步"},
    // Choose the source: the attached volumes (read off the window's
    // thread), a folder chosen, typed or pasted.
    {"Choose a volume to register it as a whole, or a folder in one.", "選一個磁碟區（整個註冊），或選其中的一個資料夾。"},
    {"Attached volumes", "已連接的磁碟區"},
    {"Read the attached volumes again", "重新讀取已連接的磁碟區"},
    {"Reading the attached volumes...", "正在讀取已連接的磁碟區……"},
    {"Cannot list the attached volumes: {}", "無法列出已連接的磁碟區：{}", Match::FormatOnly},
    {"No volume is attached.", "沒有已連接的磁碟區。"},
    {"Volume without a label", "沒有標籤的磁碟區"},
    {"{} used", "已使用 {}", Match::FormatOnly},
    {"{}, {}", "{}、{}", Match::FormatOnly},
    {"{} ({})", "{}（{}）", Match::FormatOnly},
    {"the filesystem of {}, behind another enclosure or on another device than recorded ({})",
        "{} 的檔案系統，但外接盒或裝置與記錄中的不同（{}）", Match::FormatOnly},
    {"an unfinished registration of {} ({})", "{} 未完成的註冊（{}）", Match::FormatOnly},
    {"Holds {}", "已有 {}", Match::FormatOnly},
    {"{}; the whole volume cannot be registered: choose a folder in it that does not overlap them",
        "{}；整個磁碟區不能註冊，可以選其中不重疊的資料夾", Match::FormatOnly},
    // ... and in the relocation dialog (src/gui/relocate_dialog.cpp): what
    // lies on a volume besides the archive relocated, and where that archive
    // lies on it, as recorded or its filesystem on another device.
    {"{}; the whole volume cannot be the new root: choose a folder in it that does not overlap them",
        "{}；整個磁碟區不能作為新的根目錄，可以選其中不重疊的資料夾", Match::FormatOnly},
    {"{}; nothing on it can be the new root, which would overlap them",
        "{}；其中沒有任何位置能作為新的根目錄，否則會與它們重疊", Match::FormatOnly},
    {"The filesystem of this archive is here, on another device than recorded ({})",
        "此封存的檔案系統在這裡，但裝置與記錄中的不同（{}）", Match::FormatOnly},
    {"This archive is here, as recorded ({})", "此封存在這裡，與記錄相同（{}）", Match::FormatOnly},
    {"Use as the new root", "設為新的根目錄"},
    {"Use {} as the new root", "將 {} 設為新的根目錄", Match::FormatOnly},
    {"Choose a folder in {}...", "選擇 {} 裡的資料夾…", Match::FormatOnly},
    // The button of a volume known by its GUID path alone, too long for it.
    {"Choose a folder in it...", "選擇其中的資料夾…"},
    {"Folder", "資料夾"},
    {"You can also type or paste a folder or volume, including a volume's GUID path.",
        "也可以直接輸入或貼上資料夾或磁碟區，包括磁碟區的 GUID 路徑。"},
    {"Choose a folder...", "選擇資料夾…"},
    {"This folder or volume cannot be registered: {}", "無法註冊此資料夾或磁碟區：{}", Match::FormatOnly},
    {"Cannot identify the volume of this folder or volume: {}", "無法辨識此資料夾或磁碟區所在的磁碟區：{}",
        Match::FormatOnly},
    {"This folder or volume overlaps the root of {}: archive roots on one volume may not contain each other. "
        "Choose a folder that does not overlap it.",
        "此資料夾或磁碟區與 {} 的根目錄重疊：同一磁碟區上的封存根目錄不得互相包含。請選擇不與它重疊的資料夾。",
        Match::FormatOnly},
    // Options: those that apply to the source chosen.
    {"Label", "標籤"},
    {"Leave it empty to take the repository's next label (ARCH-NNNN).", "留空時使用儲存庫的下一個標籤（ARCH-NNNN）。"},
    {"Leave it empty to take {}, the repository's next label.", "留空時使用儲存庫的下一個標籤 {}。",
        Match::FormatOnly},
    {"A label has the form ARCH-NNNN, such as ARCH-0012.", "標籤的格式是 ARCH-NNNN，例如 ARCH-0012。"},
    {"Create a new revision", "建立新版次"},
    {"Reads all of {} again and makes its current content the new baseline. Use it after you changed the content on "
        "purpose.",
        "重新讀取整個 {}，以它現在的內容作為新的基準；用於內容有意更改之後。", Match::FormatOnly},
    {"This folder or volume is registered as {}. Another archive gets revisions and a history of its own.",
        "此資料夾或磁碟區已註冊為 {}。另一個封存會有自己的版次與驗證歷程。", Match::FormatOnly},
    {"The filesystem of this folder or volume is that of {}, on another device than the one recorded. Register it as "
        "another archive only if it is another device, such as a sector-by-sector clone.",
        "此資料夾或磁碟區的檔案系統屬於 {}，但所在的裝置與記錄中的不同。只有在它是另一個裝置（例如逐磁區複製出來的"
        "複本）時，才註冊為另一個封存。",
        Match::FormatOnly},
    {"Choose whether to create a new revision of the archive registered here or to register another archive.",
        "請選擇要為已在此註冊的封存建立新版次，或註冊為另一個封存。"},
    {"This folder or volume is registered already: choose to register another archive, or choose another folder or "
        "volume.",
        "此資料夾或磁碟區已經註冊過：請選擇註冊為另一個封存，或選擇其他資料夾或磁碟區。"},
    // Why no new revision is offered at an archive's root (decision 0003;
    // AMBIGUOUS).
    {"Several archives are registered here ({}), so none takes a new revision: register another archive, or choose "
        "another folder or volume.",
        "此處註冊了多個封存（{}），所以都不能建立新版次：請註冊為另一個封存，或選擇其他資料夾或磁碟區。",
        Match::FormatOnly},
    {"{} is retired and takes no new revision: register another archive, or choose another folder or volume.",
        "{} 已停用，不能建立新版次：請註冊為另一個封存，或選擇其他資料夾或磁碟區。", Match::FormatOnly},
    // MIGRATED: the filesystem of an archive behind another enclosure or on
    // another device than recorded.
    {"The filesystem of this folder or volume is a registered archive's, but the enclosure or device differs from the "
        "one recorded: choose to register another archive, or choose another folder or volume.",
        "此資料夾或磁碟區的檔案系統屬於一個已註冊的封存，但外接盒或裝置與記錄中的不同：請選擇註冊為另一個封存，"
        "或選擇其他資料夾或磁碟區。"},
    // PREVIOUS_CARRIER (decision 0004), such as the disk a relocation left.
    {"This folder or volume is an earlier carrier of {}: an earlier revision of it was registered here, and its latest "
        "revision is on another volume. Register it as another archive only if it is to be kept and verified apart "
        "from {}.",
        "此資料夾或磁碟區是 {} 先前的載體：它較早的版次在這裡註冊，最新的版次在另一個磁碟區上。只有在要把它和 {} "
        "分開保存與驗證時，才註冊為另一個封存。",
        Match::FormatOnly},
    {"This folder or volume is an earlier carrier of a registered archive: choose to register another archive, or "
        "choose another folder or volume.",
        "此資料夾或磁碟區是一個已註冊封存先前的載體：請選擇註冊為另一個封存，或選擇其他資料夾或磁碟區。"},
    // Create a new revision... of an archive that is not at the root its
    // latest revision recorded.
    {"{} is not at {} now: its volume may be attached at another drive letter or mount point, or not attached. "
        "Choose the folder or volume where it is now.",
        "{} 目前不在 {}：它的磁碟區可能換了磁碟機代號或掛載點，或沒有連接。請選擇它現在所在的資料夾或磁碟區。",
        Match::FormatOnly},
    {"Resume the interrupted registration", "繼續中斷的註冊"},
    {"An interrupted registration of {} exists at this folder or volume: it continues without reading again the "
        "files it hashed.",
        "此資料夾或磁碟區有 {} 中斷的註冊：繼續時，已計算雜湊的檔案不必重讀。", Match::FormatOnly},
    {"Writes the manifest into the .veriarchive folder in the archive's root. Otherwise VeriArchive writes nothing to "
        "the archive.",
        "會把清單檔寫入封存根目錄的 .veriarchive 資料夾。不開啟的話，VeriArchive 不會對封存寫入任何內容。"},
    {"Copy of which archive (optional)", "這是哪個封存的副本（選填）"},
    {"Not a copy", "不是副本"},
    // Confirm: what the registration will do.
    {"Source", "來源"},
    {"To read", "要讀取"},
    {"Media copy", "媒體副本"},
    {"Copy of", "副本來源"},
    {"{}, as the interrupted registration planned it", "{}（中斷的註冊所定的標籤）", Match::FormatOnly},
    {"the rest of the interrupted registration of {}", "繼續 {} 中斷的註冊", Match::FormatOnly},
    {"{}, the archive's own", "{}（封存原有的標籤）", Match::FormatOnly},
    {"a new revision of {}", "{} 的新版次", Match::FormatOnly},
    {"the next label of the repository", "儲存庫的下一個標籤"},
    {"{}, the repository's next label", "{}（儲存庫的下一個標籤）", Match::FormatOnly},
    {"the files that the interrupted registration has not hashed yet; those it hashed are not read again",
        "中斷的註冊尚未計算雜湊的檔案；已計算雜湊的不必重讀"},
    {"another archive, besides {}", "{} 以外的另一個封存", Match::FormatOnly},
    {"a new archive", "新的封存"},
    {"every file of the volume, about {} in use; their number is known after the scan",
        "磁碟區中的每個檔案，約 {}；檔案數掃描後才知道", Match::FormatOnly},
    {"every file of the volume; their number is known after the scan", "磁碟區中的每個檔案；檔案數掃描後才知道"},
    {"every file in this folder; their number and size are known after the scan",
        "此資料夾中的每個檔案；檔案數與大小掃描後才知道"},
    {"written to the .veriarchive folder in the root", "寫入根目錄的 .veriarchive 資料夾"},
    {"not written: nothing is written to the archive", "不寫入：不會對封存寫入任何內容"},
    {"not a copy", "不是副本"},
    {"Once it starts, the time left is estimated from the actual read speed. While it registers, the computer does "
        "not go to sleep by itself; you may leave, and a notification says when it ends while the window is not in "
        "front. Stopping it halfway is fine: it can be resumed later without reading again what it read.",
        "開始後會依實際讀取速度估計剩餘時間。註冊期間電腦不會自己進入睡眠；您可以離開，結束時若視窗不在前景會發出通知。"
        "中途停止也沒關係，之後可以繼續，已讀過的不必重讀。"},
    {"Start registration", "開始註冊"},
    // The toasts of its job, and of a comparison's.
    {"{}: started", "{}：已開始", Match::FormatOnly},
    {"Registered as {}", "已註冊為 {}", Match::FormatOnly},
    {"Registered revision {} of {}", "已註冊 {1} 的版次 {0}", Match::FormatOnly},
    {"View archive", "查看封存"},
    // src/gui/verification.cpp: a FULL verification and its report.
    {"FULL VERIFY {}", "完整驗證 {}"},
    {"{} ({}) revision {}", "{}（{}）版次 {}", Match::FormatOnly},
    {"Root: {}", "根目錄：{}"},
    {"Identity: {}", "媒體身分：{}"},
    {"Coverage: {} of {} registered bytes read and matched",
        "涵蓋範圍：已註冊的 {1} 中，有 {0} 已讀取且相符"},
    {"Items:", "項目："},
    {"Media: not read, as the manifest of this revision cannot be used", "媒體：未讀取，因為這個版次的清單檔無法使用"},
    {"Failure: {}", "失敗：{}"},
    {"The run was interrupted; continue it with Resume in the archive's details in Archives, without reading again "
     "the files verified.",
        "此次執行已中斷；在「封存」頁此封存的詳細資料中按「繼續」即可接續，不必重新讀取已驗證的檔案。"},
    {"Resumed from run: {}", "已從執行 {} 繼續"},
    {"Run: {}", "執行：{}"},
    {"Choose the folder or volume to verify.", "請選擇要驗證的資料夾或磁碟區。"},
    {"Not verified: {}", "未能驗證：{}"},
    // The filesystem of an archive on another device than recorded
    // (MIGRATED), verified by folder: the window relocates it (decision 0046).
    {"Not verified: the filesystem of this folder or volume is that of archive {}, but on another device than the one "
     "recorded (MIGRATED).",
        "未能驗證：這個資料夾或磁碟區的檔案系統屬於封存 {}，但裝置與記錄中的不同（已遷移）。", Match::FormatOnly},
    {"If it is that archive's device in another enclosure, or you moved the archive to that device on purpose, "
     "relocate the archive to it with Relocate to a new carrier... in the menu of its details.",
        "如果那是該封存的裝置換了外接盒，或是您有意把封存移到那個裝置，請在它的詳細資料選單中按「搬移到新的載體…」把"
        "封存搬移過去。"},
    // An archive that more than one attached volume holds (decision 0022,
    // point 7), verified in Archives by its uuid: "Verify a folder or
    // volume..." verifies the one meant.
    {"Not verified: archive {} is on more than one attached volume, as a device and a sector-by-sector clone of it "
        "would be:{}\nVeriArchive cannot tell which one to verify. Choose \"Verify a folder or volume...\" in the "
        "menu beside \"FULL verify\" in the archive's details, and choose the folder of the one to verify; or detach "
        "the other volumes and verify the archive again.",
        "未能驗證：封存 {} 位於多個已連接的磁碟區上，例如一個裝置和它逐磁區複製出來的複本：{}\n程式無法判斷要驗證哪一個。"
        "請在此封存的詳細資料中，從「完整驗證」旁的選單選擇「驗證一個資料夾或磁碟區…」，再選擇要驗證的那一個的資料夾；"
        "或讓其他磁碟區中斷連線後，再驗證一次這個封存。",
        Match::FormatOnly},
    // src/app/verify_report.cpp: how to take a baseline for Unbaselined
    // entries (decision 0010), on the command line and in the window.
    {"The Unbaselined entries could not be read when the archive was registered, so they have no baseline yet. To "
        "take one for them, register a new revision of the archive: veriarchive-cli register {} --new-revision",
        "這些「無基準」項目在註冊此封存時無法讀取，所以還沒有基準。若要為它們建立基準，請註冊此封存的新版次："
        "veriarchive-cli register {} --new-revision",
        Match::FormatOnly},
    {"The Unbaselined entries could not be read when the archive was registered, so they have no baseline yet. To "
        "take one for them, register a new revision of the archive: choose \"{}\" in the menu of its details in "
        "Archives (or choose its folder or volume ({}) in Register Archive and turn on \"{}\"), then press \"{}\" and "
        "\"{}\".",
        "這些「無基準」項目在註冊此封存時無法讀取，所以還沒有基準。若要為它們建立基準，請註冊此封存的新版次：在"
        "「封存」頁此封存詳細資料的選單中選擇「{}」（或在「註冊封存」選擇它的資料夾或磁碟區（{}），開啟「{}」），"
        "再按「{}」和「{}」。",
        Match::FormatOnly},
    // Adding chunk hashes (decision 0033): the item of the menu of FULL
    // verify, offered for an archive without them, and the report's line.
    {"FULL verify, and add chunk hashes", "完整驗證，並加入區塊雜湊"},
    {"Chunk hashes added: revision {} records them (chunk size {}, {} chunks, Merkle root {})",
        "已加入區塊雜湊：由版次 {} 記錄（區塊大小 {}，{} 個區塊，總碼 {}）"},
    {"Chunk hashes not added: {}", "未加入區塊雜湊：{}", Match::Cause},
    // src/app/enclosure_words.cpp: a carrier seen behind another enclosure
    // (decision 0047): the question the FULL verify asks first, a read of the
    // device's own serial, and the operator confirmation (由您確認).
    {"The enclosure of {} differs from the one recorded", "{} 的外接盒與記錄中的不同", Match::FormatOnly},
    {"The filesystem ID of this volume is the one {} records, but the serial its enclosure reports has changed: "
     "recorded {}, now {}.",
        "這個磁碟區的檔案系統 ID 和 {} 記錄中的相同，但外接盒回報的序號換了：記錄中是 {}，這次是 {}。",
        Match::FormatOnly},
    {"{} records the device's own serial {}. Reading the serial now tells whether this is the same device.",
        "{} 記錄了裝置本身的序號 {}。讀取這次的序號，就能判斷是不是同一個裝置。", Match::FormatOnly},
    {"It may be the same device in another enclosure, or a sector-by-sector clone of it. A clone holds exactly the "
     "same content and passes a FULL verify too, so the verification itself cannot tell them apart.",
        "可能是同一個裝置換了外接盒，也可能是逐磁區複製出來的複本。複本的內容完全相同，完整驗證也會通過，所以驗證本身"
        "無法分辨。"},
    {"{} records no serial of the device itself, so a serial read now has nothing to be compared with: you compare "
     "it with the serial on the device's label. Only once you confirm that it is the same device and the FULL verify "
     "passes is it recorded; when the enclosure changes again, the serial tells.",
        "{} 沒有記錄裝置本身的序號，所以這次讀到的序號沒有紀錄可以比對，要由您對照裝置外殼標籤上的序號判斷。您確認是"
        "同一個裝置、完整驗證也通過之後，才會記下它；之後再換外接盒，就能用序號判斷。",
        Match::FormatOnly},
    {"Needs administrator permission: Windows asks whether to allow the VeriArchive device identity reader of an "
     "unknown publisher. It reads only the model, serial and WWN that the device reports, and reads or writes none "
     "of the data on the device.",
        "需要系統管理員權限：Windows 會詢問是否允許「不明發行者」的 VeriArchive 裝置身分讀取程式。它只讀取裝置回報的"
        "型號、序號與 WWN，不讀取或寫入裝置上的任何資料。"},
    {"Administrator permission was not given, so the device's own serial was not read. Choose \"Confirm without "
     "reading the serial\" only if you are sure that you moved this device into the new enclosure yourself.",
        "沒有取得系統管理員權限，未讀取裝置本身的序號。只有在您確定是自己把這個裝置換到新的外接盒時，才選「不讀序號，"
        "自行確認」。"},
    {"The device's own serial cannot be read: this enclosure does not pass ATA commands on. Try another enclosure; "
     "when you are sure that it is the same device, you can also confirm it yourself.",
        "讀不到裝置本身的序號：這個外接盒不轉送 ATA 指令。可以換一個外接盒後再試；確定是同一個裝置時，也可以自行確認。"},
    {"Confirm without reading the serial", "不讀序號，自行確認"},
    {"Do this only if you are sure that you moved the device of {} into the new enclosure yourself.",
        "只有在您確定是自己把 {} 的裝置換到新的外接盒時，才這麼做。", Match::FormatOnly},
    {"VeriArchive verifies it in full once. Whatever the result, the history records that you confirmed it without "
     "reading its serial; only when the verification passes is the new enclosure recorded. A sector-by-sector clone "
     "passes the verification too, so VeriArchive cannot find such a mistake for you.",
        "VeriArchive 會完整驗證一次。無論結果如何，歷程都會記錄這次是由您確認、沒有讀取序號；驗證通過之後，才會記下新的"
        "外接盒。逐磁區複製出來的複本也會通過驗證，所以 VeriArchive 無法替您發現弄錯的情況。"},
    {"What the device reports", "讀到的裝置"},
    {"{} records no serial of the device itself, so VeriArchive cannot tell by itself. Compare it with the serial on "
     "the device's label: is this the device of {}, only in another enclosure?",
        "{} 沒有記錄裝置本身的序號，所以 VeriArchive 無法自己判斷。請對照裝置外殼標籤上的序號：這是 {} 的裝置、只是"
        "換了外接盒嗎？",
        Match::FormatOnly},
    {"Once you answer yes, it is verified in full once. Whatever the result, the history records that you confirmed "
     "it after reading its serial; only when the verification passes are the new enclosure and the device's own "
     "serial recorded.",
        "回答「是」之後會完整驗證一次。無論結果如何，歷程都會記錄這次是讀取序號後由您確認；驗證通過之後，才會記下新的"
        "外接盒與裝置本身的序號。"},
    {"This is not the device of {}", "這不是 {} 的裝置", Match::FormatOnly},
    {"Its filesystem is that of {} (the same filesystem ID), but the device differs: most likely a sector-by-sector "
     "clone. You can register it as another archive (a copy of {}), then compare the two with Compare.",
        "它的檔案系統和 {} 的相同（檔案系統 ID 一樣），但裝置不同，很可能是逐磁區複製出來的複本。可以把它註冊為另一個"
        "封存（{} 的副本），再用「比較」查看兩者的內容是否相同。",
        Match::FormatOnly},
    {"Nothing was recorded.", "沒有記錄任何內容。"},
    {"The device's own serial matches ({}). Once the FULL verify passes, the new enclosure is recorded.",
        "裝置本身的序號相符（{}）。完整驗證通過後會記下新的外接盒。", Match::FormatOnly},
    {"the device's serial matches", "裝置本身的序號相符"},
    {"confirmed by you after reading the serial", "讀取序號後由您確認"},
    {"confirmed by you, serial not read", "由您確認、未讀序號"},
    {"MIGRATED: the enclosure differs from the one recorded ({})", "已遷移：外接盒與記錄中的不同（{}）",
        Match::FormatOnly},
    // ... and what the run recorded of the new carrier, in the window's report.
    {"New enclosure recorded: revision {} records it ({})", "已記下新的外接盒：由版次 {} 記錄（{}）"},
    {"New enclosure not recorded: {}", "未記下新的外接盒：{}", Match::Cause},
    // src/gui/compare_dialog.cpp, src/gui/compare_choices.cpp: the comparison
    // dialog (decisions 0020, 0046) and the report of its job.
    {"ARCHIVES {}", "比較結果：{}"},
    {"Every entry of A is in B with the same type and content, and B has no other entries.",
        "A 的每個項目都以相同的類型與內容存在於 B，且 B 沒有其他項目。"},
    {"No entry differs, but the entries listed below are not proven the same.",
        "沒有項目不同，但下列項目無法證明相同。"},
    {"A (reference): {} ({}) revision {}, recorded {}", "A（參考）：{}（{}）版次 {}，記錄於 {}"},
    {"B (copy): {} ({}) revision {}, recorded {}", "B（副本）：{}（{}）版次 {}，記錄於 {}"},
    {"Entries: {} in A, {} in B, {} the same", "項目：A 有 {} 個，B 有 {} 個，其中 {} 個相同"},
    {"Differences: {}", "差異：{}"},
    {"... and {} more differences, not listed here", "……另有 {} 個差異未列於此"},
    {"... and {} more difference, not listed here", "……另有 {} 個差異未列於此"},
    {"This compares what each registration read. FULL verification of each archive proves that "
        "its media still holds it.",
        "此比較依據各封存註冊時讀取的內容。對各封存進行完整驗證，才能證明其媒體仍保有這些內容。"},
    {"Not compared: {}", "未能比較：{}"},
    {"Choose the two archives to compare.", "請選擇要比較的兩個封存。"},
    {"A and B are the same archive; choose the copy to check as B.",
        "A 與 B 是同一個封存；請將要檢查的副本選為 B。"},
    {"Comparison checks that two registered archives hold the same entries with the same "
        "content, using their manifests in the repository; no media is read. Choose the reference "
        "archive as A and the copy to check as B.",
        "比較會使用儲存庫中的清單檔，檢查兩個已註冊的封存是否有相同的項目與內容；不會讀取媒體。"
        "請將參考封存選為 A，要檢查的副本選為 B。"},
    {"Reference archive (A)", "參考封存（A）"},
    {"Copy to check (B)", "要檢查的副本（B）"},
    {"The copies of A come first.", "A 的副本排在最前面。"},
    {"Compare", "比較"},
    {"stopped", "已停止"},
    {"not compared", "未能比較"},
    // src/gui/relocate_dialog.cpp, src/gui/archive_actions.cpp: the
    // relocation dialog (decisions 0004, 0046), the job's title and its toast;
    // the rest of its words are Register Archive's and the relocation's own.
    {"Relocate to a new carrier", "搬移到新的載體"},
    {"VeriArchive copies nothing: first copy the archive's files to the new carrier with your own tools, then "
     "choose here the folder or volume that holds them.",
        "VeriArchive 不會複製任何東西：請先用您自己的工具把封存的檔案複製到新的載體，再在這裡選擇存放這些檔案的資料夾"
        "或磁碟區。"},
    // ... where it resumes a stopped relocation (decision 0026).
    {"Resume the relocation", "繼續搬移"},
    {"Resume relocation", "繼續搬移"},
    {"The relocation of this archive to {} stopped. Resume relocation continues it without reading again what "
     "matched; where the new carrier is attached at another drive letter or mount point now, give its root below.",
        "此封存搬移至 {} 的作業已停止。按「繼續搬移」即可接續，已相符的不必重讀；如果新的載體現在接在另一個磁碟機代號"
        "或掛載點，請在下方輸入它的根目錄。",
        Match::FormatOnly},
    {"The relocation reads the new carrier in full and accepts it only when every entry matches revision {}; it "
     "then records revision {} with the new carrier, and the archive keeps its UUID and its history.",
        "搬移時會完整讀取新的載體，只有在每個項目都與版次 {} 相符時才接受它；接著以新的載體記錄版次 {}，封存保留它的 "
        "UUID 與歷程。",
        Match::FormatOnly},
    {"New root", "新根目錄"},
    {"The folder or volume on the new carrier that holds the archive's files now. You can also type or paste its "
     "path, including a volume's GUID path.",
        "新載體上現在存放封存檔案的資料夾或磁碟區。也可以直接輸入或貼上路徑，包括磁碟區的 GUID 路徑。"},
    {"Choose the new root of the archive", "選擇封存的新根目錄"},
    {"What the relocation does", "這次搬移會做的事"},
    {"Start relocation", "開始搬移"},
    {"Recorded root", "記錄中的根目錄"},
    {"not chosen yet", "尚未選擇"},
    {"Reads", "讀取"},
    {"{} ({}), each in full", "{}（{}），每個都完整讀取", Match::FormatOnly},
    {"Records", "記錄"},
    {"revision {} with the new carrier, once every entry matches", "所有項目都相符時，以新的載體記錄版次 {}",
        Match::FormatOnly},
    {"Relocate {} to {}", "把 {} 搬移至 {}", Match::FormatOnly},
    {"Resume the relocation of {} to {}", "繼續把 {} 搬移至 {}", Match::FormatOnly},
    {"{}: relocated as revision {}", "{}：已搬移，成為版次 {}", Match::FormatOnly},
    {"Relocation of {}: stopped", "{} 的搬移：已停止", Match::FormatOnly},
    {"Not relocated: {}", "未能搬移：{}", Match::FormatOnly},
    // The details' notice of a stopped relocation (src/gui/archive_rows.cpp).
    {"The relocation of this archive to {} stopped at {}. Resume continues it without reading again what matched.",
        "此封存搬移至 {} 的作業停止於 {}。按「繼續」即可接續，已相符的不必重讀。", Match::FormatOnly},
    // src/gui/export_dialog.cpp, src/gui/archive_actions.cpp: the export of an
    // archive's manifest (R-87, decision 0046), into a folder of its own.
    {"Choose the folder to export {} into", "選擇要把 {} 匯出到哪個資料夾", Match::FormatOnly},
    {"{} lies in the root of {} ({}): an export there would add files that its next FULL verification reports as "
     "added. Choose a folder outside every archive.",
        "{} 位於 {} 的根目錄（{}）中：匯出到那裡會多出檔案，下一次完整驗證會把它們報告為新增的項目。請選擇不在任何封存"
        "中的資料夾。",
        Match::FormatOnly},
    {"Export the manifest of {}", "匯出 {} 的清單檔", Match::FormatOnly},
    {"Exported the manifest of revision {} of {}", "已匯出 {1} 版次 {0} 的清單檔", Match::FormatOnly},
    {"Identity file: {}", "身分檔案：{}", Match::FormatOnly},
    {"This repository or another takes it in with `import <file.vam>` on the command line, also to put back a lost "
     "manifest; it is checked there against the revisions that repository holds.",
        "此儲存庫或另一個儲存庫可以在命令列以 `import <file.vam>` 匯入它，也可藉此放回無法取回的清單檔；匯入時會與該"
        "儲存庫已有的版次比對。"},
    {"Exported the manifest of {}", "已匯出 {} 的清單檔", Match::FormatOnly},
    {"Export of {}: stopped", "{} 的匯出：已停止", Match::FormatOnly},
    {"Not exported: {}", "未能匯出：{}", Match::FormatOnly},
    // src/gui/retire_dialog.cpp, src/gui/archive_actions.cpp: retiring an
    // archive (decisions 0003, 0046): the question, its confirmation for a
    // lost manifest, the steps and their refusals.
    {"Retire {}?", "要停用 {} 嗎？", Match::FormatOnly},
    {"Retire", "停用"},
    {"A retired archive is no longer expected to be verified and is never overdue. It keeps its verification "
     "history and is still recognised when attached, but it takes no new revisions. A retired archive cannot be "
     "made active again.",
        "已停用的封存不必再驗證，也不會逾期。它保留驗證歷程，接上時仍會被辨識，但不再接受新的版次。已停用的封存無法再"
        "恢復使用。"},
    {"`import` and `register --lost-manifest` are commands of the command line.",
        "`import` 與 `register --lost-manifest` 都是命令列的命令。"},
    {"A retired archive cannot be made active again.", "已停用的封存無法再恢復使用。"},
    {"I confirm that this manifest is lost for good and the device is gone",
        "我確認這個清單檔已無法取回，而且裝置也已不在"},
    {"Retired {}", "已停用 {}", Match::FormatOnly},
    {"Reading the archive {} ...", "正在讀取封存 {}……", Match::FormatOnly},
    {"Retiring {} ...", "正在停用 {}……", Match::FormatOnly},
    {"Not retired: {}", "未能停用：{}", Match::FormatOnly},
    {"Another step of the window is running; wait for it to end.", "視窗的另一個步驟正在執行；請等它結束。"},
    {"Its latest manifest was found lost meanwhile: choose Retire archive... again, which asks for the confirmation "
     "that this needs.",
        "這段期間發現它最新的清單檔已無法取回：請再按一次「停用封存…」，它會請您做出所需的確認。"},

    // Engine and repository messages
    // src/core/engine/register_engine.cpp
    {"the root overlaps registered archive {} (volume root); "
        "archive roots on one volume may not contain each other",
        "根目錄與已註冊的封存 {}（磁碟區根目錄）重疊；"
        "同一磁碟區上的封存根目錄不得互相包含"},
    {"the root overlaps registered archive {} (root \"{}\"); "
        "archive roots on one volume may not contain each other",
        "根目錄與已註冊的封存 {}（根目錄「{}」）重疊；"
        "同一磁碟區上的封存根目錄不得互相包含"},
    {"this media is an earlier carrier of archive {} (volume root); "
        "pass --force-new-archive to register it separately",
        "此媒體是封存 {}（磁碟區根目錄）先前的載體；若要另行註冊，"
        "請加上 --force-new-archive"},
    {"this media is an earlier carrier of archive {} (root \"{}\"); "
        "pass --force-new-archive to register it separately",
        "此媒體是封存 {}（根目錄「{}」）先前的載體；若要另行註冊，"
        "請加上 --force-new-archive"},
    {"this filesystem belongs to archive {} (volume root) but the device differs (MIGRATED); "
        "use `archive relocate`, or pass --force-new-archive",
        "此檔案系統屬於封存 {}（磁碟區根目錄），但裝置不同（已遷移）；請使用 `archive relocate`，"
        "或加上 --force-new-archive"},
    {"this filesystem belongs to archive {} (root \"{}\") but the device differs (MIGRATED); "
        "use `archive relocate`, or pass --force-new-archive",
        "此檔案系統屬於封存 {}（根目錄「{}」），但裝置不同（已遷移）；請使用 `archive relocate`，"
        "或加上 --force-new-archive"},
    {"archive {} (volume root) is retired and takes no new revisions",
        "封存 {}（磁碟區根目錄）已停用，不再接受新的版次"},
    {"archive {} (root \"{}\") is retired and takes no new revisions",
        "封存 {}（根目錄「{}」）已停用，不再接受新的版次"},
    {"several archives match this root (AMBIGUOUS); resolve the clone first",
        "有多個封存符合此根目錄（無法確定）；請先處理複製的媒體"},
    {"already registered as archive {} (volume root); "
        "pass --new-revision to re-baseline it or --force-new-archive",
        "已註冊為封存 {}（磁碟區根目錄）；請加上 --new-revision 重新建立基準，"
        "或加上 --force-new-archive"},
    {"already registered as archive {} (root \"{}\"); "
        "pass --new-revision to re-baseline it or --force-new-archive",
        "已註冊為封存 {}（根目錄「{}」）；請加上 --new-revision 重新建立基準，"
        "或加上 --force-new-archive"},
    {"--new-revision: no registered archive at this root",
        "--new-revision：此根目錄沒有已註冊的封存"},
    {"labels have the form ARCH-NNNN", "標籤的格式為 ARCH-NNNN"},
    {"--copy-of needs an archive uuid", "--copy-of 需要一個封存 uuid"},
    // --copy-of named by the start of an archive's uuid (decision 0005).
    {"--copy-of {}: no archive's uuid starts with it; give the original's uuid or its first 8 or more characters, as "
        "`archive list` shows them (`archive show <uuid>` shows the whole uuid)",
        "--copy-of {}：沒有任何封存的 uuid 以它開頭；請填原始封存的 uuid，或它的前 8 個以上的字元，也就是 "
        "`archive list` 列出的字元（`archive show <uuid>` 會列出完整的 uuid）"},
    {"--copy-of {}: several archives' uuids start with it; give more of the original's uuid, which `archive show "
        "<uuid>` shows whole",
        "--copy-of {}：有多個封存的 uuid 以它開頭；請多填一些原始封存的 uuid（`archive show <uuid>` 會列出完整的 uuid）"},
    // A root that holds the repository, or lies inside it (decision 0029, point 6).
    {"this folder or volume holds VeriArchive's repository {}, which changes while it is read, so a verification of "
        "this archive could never pass; register a folder that does not hold the repository, or move the repository "
        "out of it first",
        "此資料夾或磁碟區內含 VeriArchive 的儲存庫 {}，而儲存庫在讀取期間會變動，因此這個封存的驗證永遠無法通過；"
        "請註冊不含儲存庫的資料夾，或先把儲存庫移出此處"},
    {"this folder lies inside VeriArchive's repository {}, which changes while it is read, so a verification of this "
        "archive could never pass; register a folder outside the repository",
        "此資料夾位於 VeriArchive 的儲存庫 {} 之內，而儲存庫在讀取期間會變動，因此這個封存的驗證永遠無法通過；"
        "請註冊儲存庫以外的資料夾"},
    {"cannot identify the volume of the root: {}", "無法識別根目錄所在的磁碟區：{}", Match::Cause},
    {"there is no interrupted registration of this root to resume", "此根目錄沒有可繼續的中斷註冊"},
    {"the archive of the interrupted registration is missing", "找不到中斷的註冊所屬的封存"},
    {"an interrupted registration of this root exists (archive {}); finish it with --resume, or abandon it "
        "with `archive abandon-registration {}`",
        "此根目錄有一個中斷的註冊（封存 {}）；請以 --resume 完成它，或以 `archive abandon-registration {}` 放棄它"},
    {"the archive to re-baseline is missing from the database",
        "資料庫中找不到要重新建立基準的封存"},
    {"cannot write the media copy: {}", "無法寫入媒體副本：{}", Match::Cause},
    // The resume names the root the registration reads (decision 0022, amended 2026-10-27).
    {"registration was interrupted while scanning; continue with `register {} --resume`",
        "註冊在掃描時中斷；請以 `register {} --resume` 繼續"},
    {"the device disappeared during registration; continue with `register {} --resume`",
        "裝置在註冊期間中斷連線；請以 `register {} --resume` 繼續"},
    // A device that stopped answering while its volume stayed listed (decision 0034).
    {"the device stopped answering during registration: {} entries in a row could not be read; unplug it and "
        "attach it again; continue with `register {} --resume`; if it stops again at the same place, the "
        "device may be damaged",
        "裝置在註冊期間停止回應：連續 {} 個項目無法讀取；請拔除它再重新連接，然後以 `register {} --resume` "
        "繼續；如果它又在同一個地方停止，裝置可能已經損壞"},
    {"registration was cancelled; continue with `register {} --resume`", "註冊已取消；請以 `register {} --resume` 繼續"},
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
    {"archive {} is retired and takes no new revisions: abandon its unfinished registration with "
        "`archive abandon-registration {}`",
        "封存 {} 已停用，不再接受新的版次：請以 `archive abandon-registration {}` 放棄它未完成的註冊"},
    // ... nor one retired while the registration ran (decision 0003).
    {"archive {} was retired while this registration ran; a retired archive takes no new revisions",
        "封存 {} 在這次註冊期間被停用；已停用的封存不再接受新的版次"},
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
    {"the root overlaps the root of archive {}, whose latest manifest records this volume's filesystem id and device",
        "根目錄與封存 {} 的根目錄重疊，而該封存最新的清單檔所記錄的正是此磁碟區的檔案系統 UUID 與裝置"},
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
    {"the folder {} of archive {} was not found: its volume is attached, but the folder is not there",
        "找不到封存 {1} 的資料夾 {0}：它所在的磁碟區已連接，但資料夾不在那裡"},
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
        "relocate {} {} --resume`",
        "執行 {} 是封存 {} 未完成的搬移中斷時留下的紀錄；請以 `archive relocate {} {} --resume` 繼續"},
    {"run {} is an interrupted attempt of a relocation of archive {} that has ended; nothing is left to resume",
        "執行 {} 是封存 {} 某次搬移中斷時留下的紀錄，該搬移已結束；沒有可繼續的作業"},
    // A relocation made for an older revision never records (decision 0030).
    {"run {} is an interrupted attempt of the unfinished relocation of archive {}, which verifies revision {}, but "
        "the archive now has a later revision ({}); the relocation can no longer finish: abandon it with `archive "
        "abandon-relocation {}`, then relocate again",
        "執行 {} 是封存 {} 未完成的搬移中斷時留下的紀錄，該搬移驗證的是版次 {}，但封存現在已有較新的版次（{}）；"
        "該搬移再也無法完成：請以 `archive abandon-relocation {}` 放棄它，再重新搬移"},
    {"run {} has an invalid resume record", "執行 {} 的繼續紀錄無效"},
    // Adding chunk hashes (decision 0033): refused before anything is read.
    {"run {} adds no chunk hashes; resume it without --add-chunk-hashes, or start a new verification with it",
        "執行 {} 不會加入區塊雜湊；請不加 --add-chunk-hashes 繼續它，或加上 --add-chunk-hashes 開始新的驗證"},
    {"chunk hashes cannot be added at chunk size 0", "無法以區塊大小 0 加入區塊雜湊"},
    {"revision {} of archive {} has chunk hashes already; a FULL verify compares them, and there are none to add",
        "封存 {1} 的版次 {0} 已有區塊雜湊；完整驗證會比對它們，沒有可加入的區塊雜湊"},
    // Why a run that adds chunk hashes records no revision with them.
    {"the verification did not pass; revision {} stays without chunk hashes", "驗證未通過；版次 {} 仍不含區塊雜湊"},
    {"not every entry matches revision {} as recorded; revision {} stays without chunk hashes (`register "
        "--new-revision` records the archive as it is now, with chunk hashes)",
        "並非每個項目都與版次 {0} 的紀錄相符；版次 {1} 仍不含區塊雜湊（`register --new-revision` "
        "會依封存目前的內容記錄新版次，並含區塊雜湊）"},
    {"archive {} is at revision {} now, recorded by another operation while this verification ran",
        "封存 {} 現在是版次 {}，由其他作業在這次驗證期間記錄"},
    {"archive {} was retired while this verification ran; a retired archive takes no new revisions",
        "封存 {} 在這次驗證期間被停用；已停用的封存不再接受新的版次"},
    {"an unfinished registration of archive {} (root \"{}\") is creating revision {}; finish it with `register "
        "--resume`, or abandon it with `archive abandon-registration {}`",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}；請以 `register --resume` 完成該註冊，"
        "或以 `archive abandon-registration {}` 放棄它"},
    {"the entry lines of revision {} cannot be written again as they are; revision {} stays without chunk hashes "
        "(`register --new-revision` records them)",
        "版次 {0} 的項目行無法原樣寫出；版次 {1} 仍不含區塊雜湊（`register --new-revision` 會記錄它們）"},
    {"chunk hashes of {}: {} read, {} expected", "{} 的區塊雜湊：讀到 {} 個，應為 {} 個"},
    {"the verification was abandoned while it ran", "這次驗證在執行期間被放棄"},
    // A verification that goes on past a changed enclosure (decision 0047):
    // refused before anything is read; and why it records no new carrier.
    {"chunk hashes are not added by the verification that accepts another enclosure; verify the archive first, then "
        "add chunk hashes",
        "接受新外接盒的驗證不會加入區塊雜湊；請先驗證封存，再加入區塊雜湊"},
    {"the confirmation names the enclosure {} (recorded: {}), but archive {} is seen through the enclosure {} "
        "(recorded: {}); confirm it again",
        "確認的外接盒是 {}（記錄中：{}），但封存 {} 現在是透過外接盒 {}（記錄中：{}）看到的；請重新確認"},
    {"a confirmation by the device's serial names no device", "依裝置序號確認時，沒有提供讀到的裝置"},
    {"archive {} has no confirmed device to compare the serial {} with; only the operator can confirm it",
        "封存 {} 沒有已確認的裝置可與序號 {} 比對；只能由您確認"},
    {"the device's own serial {} is not the serial {} of the device of archive {}: it is another device",
        "裝置本身的序號 {0} 與封存 {2} 的裝置序號 {1} 不同：這是另一個裝置"},
    {"this volume is the carrier of archive {} as recorded, so it is not taken for the device of archive {}; verify "
        "archive {}, or attach the device of archive {} through another enclosure",
        "這個磁碟區是封存 {0} 記錄中的載體，所以不會當成封存 {1} 的裝置；請驗證封存 {2}，或把封存 {3} 的裝置接在另一個"
        "外接盒上"},
    // Why such a run records no new carrier (VerifyOutcome::carrier_not_recorded).
    {"the verification did not pass", "驗證未通過"},
    {"some names cannot exist on this system and would drop out of a new revision (verify the archive on a system "
        "where they can exist)",
        "有些名稱無法存在於這個系統上，新版次會遺漏它們（請在能保存這些名稱的系統上驗證封存）"},
    // One operation that creates an archive's next revision at a time: a
    // verification that adds chunk hashes refuses, and is refused by, a
    // registration and a relocation (decision 0033), and by another of its
    // kind; and abandoning it.
    {"an unfinished verification of archive {} that adds chunk hashes (run {}) is creating revision {}; nothing "
        "was changed (finish it with `verify --full {} --resume {}` first, or abandon it with `archive "
        "abandon-verification {}`)",
        "封存 {0} 有一個加入區塊雜湊的未完成驗證（執行 {1}）正在建立版次 {2}；未做任何變更（請先以 "
        "`verify --full {3} --resume {4}` 完成它，或以 `archive abandon-verification {5}` 放棄它）"},
    // The same refusal of a verification in Archives (decision 0046), which
    // names the window's own ways on in place of the command lines: Resume
    // and Abandon... on its row under Unfinished operations first, then
    // Resume in the archive's details, where its stopped verification shows.
    {"an unfinished verification of archive {} that adds chunk hashes (run {}) is creating revision {}; nothing "
        "was changed (finish it first with Resume, or abandon it with Abandon..., on its row under Unfinished "
        "operations in Archives; or finish it with Resume in the archive's details)",
        "封存 {0} 有一個加入區塊雜湊的未完成驗證（執行 {1}）正在建立版次 {2}；未做任何變更（請先在「封存」頁的"
        "「未完成的作業」中按它那一列的「繼續」完成它，或按該列的「放棄…」放棄它；也可以在此封存的詳細資料中按"
        "「繼續」完成它）"},
    // ... while that verification still runs, when none of them is offered yet.
    {"an unfinished verification of archive {} that adds chunk hashes (run {}) is creating revision {} and is still "
        "running; nothing was changed (wait for it to finish; if it stops, finish it with Resume, or abandon it with "
        "Abandon..., on its row under Unfinished operations in Archives; or finish it with Resume in the archive's "
        "details)",
        "封存 {0} 有一個加入區塊雜湊的未完成驗證（執行 {1}）正在建立版次 {2}，而且仍在執行；未做任何變更（請等它完成；"
        "若它停止了，請在「封存」頁的「未完成的作業」中按它那一列的「繼續」完成它，或按該列的「放棄…」放棄它；"
        "也可以在此封存的詳細資料中按「繼續」完成它）"},
    // The same refusal of a registration in Register Archive (decision 0033).
    {"an unfinished verification of archive {} that adds chunk hashes (run {}) is creating revision {}; nothing "
        "was changed (finish it first with Resume, or abandon it with Abandon..., on its row under Unfinished "
        "operations in Archives; or finish it with Resume in the archive's details there)",
        "封存 {0} 有一個加入區塊雜湊的未完成驗證（執行 {1}）正在建立版次 {2}；未做任何變更（請先在「封存」頁的"
        "「未完成的作業」中按它那一列的「繼續」完成它，或按該列的「放棄…」放棄它；也可以在該頁此封存的詳細資料中按"
        "「繼續」完成它）"},
    {"an unfinished verification of archive {} that adds chunk hashes (run {}) is creating revision {} and is still "
        "running; nothing was changed (wait for it to finish; if it stops, finish it with Resume, or abandon it with "
        "Abandon..., on its row under Unfinished operations in Archives; or finish it with Resume in the archive's "
        "details there)",
        "封存 {0} 有一個加入區塊雜湊的未完成驗證（執行 {1}）正在建立版次 {2}，而且仍在執行；未做任何變更（請等它完成；"
        "若它停止了，請在「封存」頁的「未完成的作業」中按它那一列的「繼續」完成它，或按該列的「放棄…」放棄它；"
        "也可以在該頁此封存的詳細資料中按「繼續」完成它）"},
    // The refusal of Register Archive and of a verification in Archives while
    // a registration of the archive is unfinished, in the windows' own words
    // (decisions 0025, 0030, 0033, 0046): Resume and Abandon... on its row
    // under Unfinished operations first, then Register Archive, the root
    // chosen there, whose options then resume it; of the root chosen, of
    // another root of the archive, or one made for an older revision, which
    // can only be abandoned; once it has stopped, and while it runs.
    {"an interrupted registration of this root exists (archive {}); finish it with Resume, or abandon it with "
        "Abandon..., on its row under Unfinished operations in Archives; or choose this folder or volume again in "
        "Register Archive, whose options then resume it",
        "此根目錄有一個中斷的註冊（封存 {}）；請在「封存」頁的「未完成的作業」中按它那一列的「繼續」完成它，"
        "或按該列的「放棄…」放棄它；也可以在「註冊封存」再選一次這個資料夾或磁碟區，在選項中繼續它"},
    {"a registration of this root (archive {}) is still running; wait for it to finish; if it stops, finish it with "
        "Resume, or abandon it with Abandon..., on its row under Unfinished operations in Archives; or choose this "
        "folder or volume again in Register Archive, whose options then resume it",
        "此根目錄的註冊（封存 {}）仍在執行；請等它完成；若它停止了，請在「封存」頁的「未完成的作業」中按它那一列的"
        "「繼續」完成它，或按該列的「放棄…」放棄它；也可以在「註冊封存」再選一次這個資料夾或磁碟區，在選項中繼續它"},
    {"an unfinished registration of archive {} (root \"{}\") is creating revision {}; nothing was changed (finish "
        "it first with Resume, or abandon it with Abandon..., on its row under Unfinished operations in Archives; or "
        "finish it by choosing its root in Register Archive, whose options then resume it)",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}；未做任何變更（請先在「封存」頁的「未完成的作業」"
        "中按它那一列的「繼續」完成它，或按該列的「放棄…」放棄它；也可以在「註冊封存」選擇它的根目錄，在選項中"
        "繼續它）"},
    {"an unfinished registration of archive {} (root \"{}\") is creating revision {} and is still running; nothing "
        "was changed (wait for it to finish; if it stops, finish it with Resume, or abandon it with Abandon..., on "
        "its row under Unfinished operations in Archives; or finish it by choosing its root in Register Archive, "
        "whose options then resume it)",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}，而且仍在執行；未做任何變更（請等它完成；"
        "若它停止了，請在「封存」頁的「未完成的作業」中按它那一列的「繼續」完成它，或按該列的「放棄…」放棄它；"
        "也可以在「註冊封存」選擇它的根目錄，在選項中繼續它）"},
    {"the unfinished registration of archive {} (root \"{}\") would create revision {}, but the archive now has a "
        "later revision ({}); the registration can no longer finish: abandon it with Abandon... under Unfinished "
        "operations in Archives",
        "封存 {} 未完成的註冊（根目錄「{}」）原本要建立版次 {}，但封存現在已有較新的版次（{}）；這個註冊再也無法完成："
        "請在「封存」頁的「未完成的作業」中按「放棄…」放棄它"},
    {"the unfinished registration of archive {} (root \"{}\") would create revision {}, but the archive now has a "
        "later revision ({}); the registration can no longer finish, and is still running: wait for it to stop, then "
        "abandon it with Abandon... under Unfinished operations in Archives",
        "封存 {} 未完成的註冊（根目錄「{}」）原本要建立版次 {}，但封存現在已有較新的版次（{}）；這個註冊再也無法完成，"
        "而且仍在執行：請等它停止，再在「封存」頁的「未完成的作業」中按「放棄…」放棄它"},
    // ... and while a relocation of the archive is unfinished: Resume on its
    // row, or in the archive's details, continues it (decision 0046).
    {"an unfinished relocation of archive {} (to \"{}\") exists; resume it with Resume, or abandon it with "
        "Abandon..., on its row under Unfinished operations in Archives; or resume it in the archive's details",
        "封存 {} 有一個未完成的搬移（至「{}」）；請在「封存」頁「未完成的作業」中它那一列按「繼續」接續，或按「放棄…」"
        "放棄它；也可以在封存的詳細資料中按「繼續」"},
    {"an unfinished relocation of archive {} (to \"{}\") exists and is still running; wait for it to finish; if it "
        "stops, resume it with Resume, or abandon it with Abandon..., on its row under Unfinished operations in "
        "Archives; or resume it in the archive's details",
        "封存 {} 有一個未完成的搬移（至「{}」），而且仍在執行；請等它完成；若它停止了，請在「封存」頁「未完成的作業」中"
        "它那一列按「繼續」接續，或按「放棄…」放棄它；也可以在封存的詳細資料中按「繼續」"},
    // A relocation made for an older revision (decision 0030): only abandoned.
    {"the unfinished relocation of archive {} (to \"{}\") verifies revision {}, but the archive now has a later "
        "revision ({}); the relocation can no longer finish: abandon it with Abandon... under Unfinished operations "
        "in Archives, then relocate again",
        "封存 {} 未完成的搬移（至「{}」）驗證的是版次 {}，但封存現在已有較新的版次（{}）；這次搬移再也無法完成："
        "請在「封存」頁的「未完成的作業」中按「放棄…」放棄它，再重新搬移"},
    {"the unfinished relocation of archive {} (to \"{}\") verifies revision {}, but the archive now has a later "
        "revision ({}); the relocation can no longer finish, and is still running: wait for it to stop, then "
        "abandon it with Abandon... under Unfinished operations in Archives, then relocate again",
        "封存 {} 未完成的搬移（至「{}」）驗證的是版次 {}，但封存現在已有較新的版次（{}）；這次搬移再也無法完成，"
        "而且仍在執行：請等它停止，再在「封存」頁的「未完成的作業」中按「放棄…」放棄它，然後重新搬移"},
    // What the window says of a relocation that stopped and keeps its saved
    // progress (src/app/refusal_in_window.cpp, decision 0046): Resume in the
    // window continues it, in place of `archive relocate ... --resume`.
    {"relocation was interrupted: the device of the new carrier stopped answering ({} entries in a row could not be "
        "read); unplug it and attach it again, then continue it with Resume under Unfinished operations in Archives; "
        "if it stops again at the same place, the device may be damaged",
        "搬移已中斷：新載體的裝置停止回應（連續 {} 個項目無法讀取）；請拔除它再重新連接，然後在「封存」頁的「未完成的"
        "作業」中按「繼續」接續；如果它又在同一個地方停止，裝置可能已經損壞",
        Match::FormatOnly},
    {"relocation was interrupted; continue it with Resume under Unfinished operations in Archives, or in the "
        "archive's details, without reading again what matched",
        "搬移已中斷；請在「封存」頁的「未完成的作業」中，或在封存的詳細資料中按「繼續」接續，已相符的不必重讀"},
    {"{}; the relocation keeps its saved progress: continue it with Resume under Unfinished operations in Archives "
        "once that is dealt with",
        "{}；搬移保留其已儲存的進度：處理好之後，請在「封存」頁的「未完成的作業」中按「繼續」接續", Match::FormatOnly},
    // A resume where the new carrier is not attached now, as when a drive
    // letter changed: the relocation dialog resumes it at the root given.
    {"the volume or folder of this root is not the one the unfinished relocation of archive {} started on (\"{}\"); "
        "where the new carrier is attached at another drive letter or mount point now, choose Relocate to a new "
        "carrier... in the archive's details and give its root there, which resumes the relocation; otherwise attach "
        "it as it was and choose Resume again, or abandon the relocation with Abandon... under Unfinished operations "
        "in Archives",
        "此根目錄所在的磁碟區或資料夾，不是封存 {} 未完成的搬移開始時的那一個（「{}」）；如果新的載體現在接在另一個"
        "磁碟機代號或掛載點，請在封存的詳細資料中按「搬移到新的載體…」並在那裡輸入它的根目錄，即可繼續搬移；否則請照"
        "原樣接上後再按「繼續」，或在「封存」頁的「未完成的作業」中按「放棄…」放棄這次搬移",
        Match::FormatOnly},
    {"{}; where the new carrier is attached at another drive letter or mount point now, choose Relocate to a new "
        "carrier... in the archive's details and give its root there, which resumes the relocation; otherwise attach "
        "it as it was and choose Resume again, or abandon the relocation with Abandon... under Unfinished operations "
        "in Archives",
        "{}；如果新的載體現在接在另一個磁碟機代號或掛載點，請在封存的詳細資料中按「搬移到新的載體…」並在那裡輸入它的"
        "根目錄，即可繼續搬移；否則請照原樣接上後再按「繼續」，或在「封存」頁的「未完成的作業」中按「放棄…」放棄這次"
        "搬移",
        Match::FormatOnly},
    // Another operation recorded a later revision before it began.
    {"{}; nothing was changed: relocate again", "{}；未做任何變更：請重新搬移", Match::FormatOnly},
    // src/app/relocation_report.cpp: its saved progress could not be removed.
    {"the new carrier does not hold the recorded content; nothing was changed, but the relocation's saved progress "
        "could not be removed: {}; abandon the relocation with Abandon... under Unfinished operations in Archives "
        "before relocating again",
        "新載體上的內容與記錄不符；未做任何變更，但無法刪除搬移已儲存的進度：{}；重新搬移之前，請在「封存」頁的"
        "「未完成的作業」中按「放棄…」放棄這次搬移",
        Match::FormatOnly},
    {"the unfinished relocation of archive {} (to \"{}\") found that the new carrier does not hold the recorded "
        "content and cannot be resumed; abandon it with Abandon... under Unfinished operations in Archives, then "
        "relocate again",
        "封存 {} 未完成的搬移（至「{}」）發現新載體上的內容與記錄不符，無法繼續；請在「封存」頁的「未完成的作業」中"
        "按「放棄…」放棄它，再重新搬移"},
    {"the unfinished relocation of archive {} (to \"{}\") found that the new carrier does not hold the recorded "
        "content and is still running; wait for it to end; if it is then still listed under Unfinished operations "
        "in Archives, abandon it there with Abandon..., then relocate again",
        "封存 {} 未完成的搬移（至「{}」）發現新載體上的內容與記錄不符，而且仍在執行；請等它結束；結束後若它仍列在"
        "「封存」頁的「未完成的作業」中，請在那裡按「放棄…」放棄它，再重新搬移"},
    {"this verification is still running in process {} ({}) on this computer; resume it once that process has "
        "ended",
        "此驗證仍在這台電腦的處理程序 {}（{}）中執行；請等該處理程序結束後再繼續"},
    {"cannot start the verification: {}", "無法開始驗證：{}", Match::Cause},
    {"archive {} has no unfinished verification that adds chunk hashes", "封存 {} 沒有加入區塊雜湊的未完成驗證"},
    {"the unfinished verification of archive {} that adds chunk hashes is no longer the one shown (it was resumed, "
        "finished or abandoned meanwhile); nothing was changed",
        "封存 {} 加入區塊雜湊的未完成驗證已不是畫面上的那一個（其間已被繼續、完成或放棄）；未做任何變更"},
    {"the verification of archive {} that adds chunk hashes is still running in process {} ({}) on this computer; "
        "abandon it once that process has ended",
        "封存 {} 加入區塊雜湊的驗證仍在這台電腦的處理程序 {}（{}）中執行；請等該處理程序結束後再放棄"},
    {"an attempt of the verification of archive {} that adds chunk hashes said it was working less than 3 minutes "
        "ago; abandon it once it has stopped",
        "封存 {} 加入區塊雜湊的驗證在不到 3 分鐘前仍回報正在執行；請等它停止後再放棄"},
    {"cannot read the unfinished verification: {}", "無法讀取未完成的驗證：{}", Match::Cause},
    {"cannot abandon the verification: {}", "無法放棄驗證：{}", Match::Cause},
    // A run that read everything keeps its saved progress when the revision
    // with chunk hashes cannot be written or recorded: its resume records it.
    {"verification finished as PASSED but its revision with chunk hashes could not be written: {}; resume the run "
        "with `verify --full {} --resume {}` to record it",
        "驗證結果為通過，但無法寫入含區塊雜湊的版次：{}；請以 `verify --full {} --resume {}` 繼續該執行，以記錄該版次",
        Match::Cause},
    {"verification finished as PASSED_WITH_WARNINGS but its revision with chunk hashes could not be written: {}; "
        "resume the run with `verify --full {} --resume {}` to record it",
        "驗證結果為通過（有警告），但無法寫入含區塊雜湊的版次：{}；請以 `verify --full {} --resume {}` "
        "繼續該執行，以記錄該版次",
        Match::Cause},
    // ... and the revision with the new carrier behind another enclosure
    // (decision 0047).
    {"verification finished as PASSED but its revision with the new enclosure could not be written: {}; resume the "
        "run with `verify --full {} --resume {}` to record it",
        "驗證結果為通過，但無法寫入記錄新外接盒的版次：{}；請以 `verify --full {} --resume {}` 繼續該執行，以記錄該版次",
        Match::Cause},
    {"verification finished as PASSED_WITH_WARNINGS but its revision with the new enclosure could not be written: "
        "{}; resume the run with `verify --full {} --resume {}` to record it",
        "驗證結果為通過（有警告），但無法寫入記錄新外接盒的版次：{}；請以 `verify --full {} --resume {}` "
        "繼續該執行，以記錄該版次",
        Match::Cause},
    {"verification finished as PASSED but could not be recorded: {}; resume the run with `verify --full {} "
        "--resume {}` to record it",
        "驗證結果為通過，但無法記錄：{}；請以 `verify --full {} --resume {}` 繼續該執行，以記錄該版次",
        Match::Cause},
    {"verification finished as PASSED_WITH_WARNINGS but could not be recorded: {}; resume the run with `verify "
        "--full {} --resume {}` to record it",
        "驗證結果為通過（有警告），但無法記錄：{}；請以 `verify --full {} --resume {}` 繼續該執行，以記錄該版次",
        Match::Cause},
    {"media copy {} differs from the repository copy", "媒體副本 {} 與儲存庫中的副本不同"},
    {"the media holds revision {}, newer than the repository's {}: "
        "the repository may have been rolled back",
        "媒體上有版次 {}，比儲存庫的版次 {} 新：儲存庫可能已被回復到較舊的狀態"},
    {"no registered archive {}", "找不到已註冊的封存 {}"},
    {"the folder {} is empty, and no archive is registered there; if a volume is mounted at this folder, check that "
        "the device that holds it is attached",
        "資料夾 {} 是空的，也沒有任何封存註冊在那裡；如果這個資料夾是某個磁碟區掛載的位置，請確認存放該磁碟區的裝置已連接"},
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
        "relocated; it is not verified",
        "先前的載體：此媒體在封存 {} 搬移前存放其較早的版次；不會驗證此媒體"},
    {"the matched archive is missing from the database", "資料庫中找不到相符的封存"},
    {"run {} cannot be resumed: the media identity or the archive revision changed since it "
        "started; run a new verification",
        "執行 {} 無法繼續：自開始以來媒體身分或封存版次已改變；請執行新的驗證"},
    {"identity: no hardware serial to compare (MATCH_WEAK)",
        "媒體身分：沒有可比對的硬體序號（弱相符）"},
    {"identity: the filesystem id cannot be read; the volume was recognised by its device (MATCH_WEAK)",
        "媒體身分：讀不到檔案系統 UUID，改以硬碟序號和容量辨識（弱相符）"},
    {"scan: {}", "掃描：{}", Match::Cause},
    // Portability warnings that the manifest records (R-42, §A6).
    {"some names differ from another name in the same folder only in upper and lower case, or in how a letter with "
        "an accent or a mark is encoded (Unicode normalisation); a volume or system that does not tell such names "
        "apart cannot keep them all",
        "有些名稱與同一資料夾中的另一個名稱只差在大小寫，或帶重音、標記的字母的編碼方式（Unicode 正規化）；"
        "不區分這些差異的磁碟區或作業系統無法同時保存它們"},
    // The program that read the media, where it is no kernel driver of its
    // format (decision 0006), noted in a verification report; and what
    // opening the repository is said to have done, once (decision 0032).
    {"note: {}", "附註：{}", Match::Cause},
    {"Note: {}", "附註：{}"},
    {"driver: {}", "驅動程式：{}"},
    // A device that went away while it was read (decision 0034).
    {"the device that holds the archive went away while it was read (unplugged, or no longer answering); attach it "
        "again, then resume this run",
        "存放此封存的裝置在讀取期間離開了（被拔除，或不再回應）；請重新連接它，再繼續這次執行"},
    // A device that stopped answering while its volume stayed listed (decision 0034).
    {"the device that holds the archive stopped answering while it was read: {} entries in a row could not be read; "
        "unplug it and attach it again, then resume this run; if it stops again at the same place, the device may be "
        "damaged",
        "存放此封存的裝置在讀取期間停止回應：連續 {} 個項目無法讀取；請拔除它再重新連接，然後繼續這次執行；"
        "如果它又在同一個地方停止，裝置可能已經損壞"},
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
    // An item's detail in plain words (app/plain_words.h); --json keeps the codes.
    {"modification time", "修改時間"},
    {"permissions", "權限模式"},
    {"attributes", "檔案屬性"},
    {"modification time, permissions", "修改時間、權限模式"},
    {"modification time, attributes", "修改時間、檔案屬性"},
    {"permissions, attributes", "權限模式、檔案屬性"},
    {"modification time, permissions, attributes", "修改時間、權限模式、檔案屬性"},
    {"link target converted by this system", "連結目標已被轉換"},
    {"link target differs", "連結目標不同"},
    {"a mount point now covers this path", "此路徑現在被掛載點覆蓋"},
    {"another volume is mounted here; it is not part of the archive", "這裡掛載了另一個磁碟區；它不屬於這個封存"},
    {"directory {} cannot be listed", "無法列出資料夾 {} 的內容"},
    {"name cannot exist on {}", "此名稱無法存在於 {}"},
    {"name cannot exist on Windows", "此名稱無法存在於 Windows"},
    {"name cannot exist on Linux", "此名稱無法存在於 Linux"},
    {"inside a directory unreadable at registration", "位於註冊時無法讀取的資料夾內"},
    {"file -> folder", "檔案 → 資料夾"},
    {"file -> link", "檔案 → 連結"},
    {"folder -> file", "資料夾 → 檔案"},
    {"folder -> link", "資料夾 → 連結"},
    {"link -> file", "連結 → 檔案"},
    {"link -> folder", "連結 → 資料夾"},
    {"unreadable since registration", "註冊時即無法讀取"},
    {"readable now, unreadable at registration", "現在可讀取，註冊時無法讀取"},
    {"size differs", "大小不同"},
    {"content differs", "內容不同"},
    // A file whose content differs, compared by chunk (decision 0032): which
    // chunks differ, and the byte where the first begins. The chunks are one
    // value ("3", "3, 5", "1-3"); the word joining the last of a list has an
    // entry of its own.
    {"content differs: chunk {} of {} differs, from byte {}", "內容不同：共 {1} 個區塊，其中區塊 {0} 不同，從位元組 {2} 開始"},
    {"content differs: chunks {} of {} differ, the first from byte {}",
        "內容不同：共 {1} 個區塊，其中區塊 {0} 不同，第一個從位元組 {2} 開始"},
    {"content differs: chunks {} and {} of {} differ, the first from byte {}",
        "內容不同：共 {2} 個區塊，其中區塊 {0} 和 {1} 不同，第一個從位元組 {3} 開始"},
    {"content differs: chunks {} and {} more of {} differ, the first from byte {}",
        "內容不同：共 {2} 個區塊，其中區塊 {0} 及另外 {1} 個區塊不同，第一個從位元組 {3} 開始"},
    // A warning of the run: a file whose content matches both its file hashes
    // while a chunk hash of it does not (decision 0032). The chunks as above.
    {"the chunk hashes of {} cannot be relied on: chunk {} of {} does not match, while the whole file matches its "
        "hashes; the file is fine, and a new revision fixes them",
        "{0} 的區塊雜湊不可靠：共 {2} 個區塊，其中區塊 {1} 不符，但整個檔案與其雜湊相符；"
        "檔案本身沒有問題，建立新版次即可修正"},
    {"the chunk hashes of {} cannot be relied on: chunks {} of {} do not match, while the whole file matches its "
        "hashes; the file is fine, and a new revision fixes them",
        "{0} 的區塊雜湊不可靠：共 {2} 個區塊，其中區塊 {1} 不符，但整個檔案與其雜湊相符；"
        "檔案本身沒有問題，建立新版次即可修正"},
    {"the chunk hashes of {} cannot be relied on: chunks {} and {} of {} do not match, while the whole file matches "
        "its hashes; the file is fine, and a new revision fixes them",
        "{0} 的區塊雜湊不可靠：共 {3} 個區塊，其中區塊 {1} 和 {2} 不符，但整個檔案與其雜湊相符；"
        "檔案本身沒有問題，建立新版次即可修正"},
    {"the chunk hashes of {} cannot be relied on: chunks {} and {} more of {} do not match, while the whole file "
        "matches its hashes; the file is fine, and a new revision fixes them",
        "{0} 的區塊雜湊不可靠：共 {3} 個區塊，其中區塊 {1} 及另外 {2} 個區塊不符，但整個檔案與其雜湊相符；"
        "檔案本身沒有問題，建立新版次即可修正"},
    // The other half: a file whose every chunk matches while a file hash of
    // it does not. The hash's name (BLAKE3, SHA-256) is a value.
    {"the whole-file {} hash of {} cannot be relied on: it does not match, while each of the file's {} chunks "
        "matches its chunk hash; the file is fine, and a new revision fixes it",
        "{1} 的整個檔案 {0} 雜湊不可靠：它不符，但檔案的 {2} 個區塊都與各自的區塊雜湊相符；"
        "檔案本身沒有問題，建立新版次即可修正"},
    {"the whole-file BLAKE3 and SHA-256 hashes of {} cannot be relied on: they do not match, while each of the "
        "file's {} chunks matches its chunk hash; the file is fine, and a new revision fixes them",
        "{0} 的整個檔案 BLAKE3 和 SHA-256 雜湊不可靠：它們不符，但檔案的 {1} 個區塊都與各自的區塊雜湊相符；"
        "檔案本身沒有問題，建立新版次即可修正"},
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
    {"a retired archive takes no new revisions", "已停用的封存不再接受新的版次"},
    {"cannot identify the volume of the new root: {}", "無法識別新根目錄所在的磁碟區：{}",
        Match::Cause},
    {"the new root overlaps archive {} on the same volume", "新根目錄與同一磁碟區上的封存 {} 重疊"},
    {"relocation was interrupted; nothing was changed", "搬移已中斷；未做任何變更"},
    // A relocation resumes (decision 0026).
    {"relocation was interrupted; continue it with `archive relocate {} {} --resume`",
        "搬移已中斷；請以 `archive relocate {} {} --resume` 繼續"},
    // The new carrier's device stopped answering while its volume stayed listed (decision 0034).
    {"relocation was interrupted: the device of the new carrier stopped answering ({} entries in a row could not be "
        "read); unplug it and attach it again, then continue it with `archive relocate {} {} --resume`; if "
        "it stops again at the same place, the device may be damaged",
        "搬移已中斷：新載體的裝置停止回應（連續 {} 個項目無法讀取）；請拔除它再重新連接，然後以 "
        "`archive relocate {} {} --resume` 繼續；如果它又在同一個地方停止，裝置可能已經損壞"},
    {"an unfinished relocation of archive {} (to \"{}\") exists; resume it with `archive relocate {} {} "
        "--resume`, or abandon it with `archive abandon-relocation {}`",
        "封存 {} 有一個未完成的搬移（至「{}」）；請以 `archive relocate {} {} --resume` 繼續，"
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
    {"revision {} of archive {} was recorded by another operation before this relocation could begin",
        "封存 {1} 的版次 {0} 已在這次搬移開始前由其他作業記錄"},
    {"cannot start the relocation: {}", "無法開始搬移：{}", Match::Cause},
    {"the unfinished relocation of archive {} was abandoned while this attempt ran; nothing was changed",
        "封存 {} 未完成的搬移已在這次執行期間被放棄；未做任何變更"},
    // A retired archive takes no new revision, also one retired while the
    // relocation ran (decision 0003): it stays retired.
    {"archive {} was retired while this relocation ran; a retired archive takes no new revisions",
        "封存 {} 在這次搬移期間被停用；已停用的封存不再接受新的版次"},
    {"an unfinished registration of archive {} (root \"{}\") is creating revision {}; this relocation recorded "
        "nothing (finish that registration with `register {} --resume`, then relocate again; or abandon it with "
        "`archive abandon-registration {}`, then continue this relocation with `archive relocate {} {} --resume`)",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}；這次搬移未記錄任何內容（請以 "
        "`register {} --resume` 完成該註冊後再重新搬移；或以 `archive abandon-registration {}` 放棄該註冊後，"
        "以 `archive relocate {} {} --resume` 繼續這次搬移）"},
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
        "{} --resume`",
        "無法掃描新根目錄：{}；搬移保留其已儲存的進度：請以 `archive relocate {} {} --resume` 繼續",
        Match::Cause},
    {"cannot write the manifest: {}; the relocation keeps its saved progress: continue it with `archive relocate {} "
        "{} --resume`",
        "無法寫入清單檔：{}；搬移保留其已儲存的進度：請以 `archive relocate {} {} --resume` 繼續",
        Match::Cause},
    {"cannot record the relocation: {}; the relocation keeps its saved progress: continue it with `archive "
        "relocate {} {} --resume`",
        "無法記錄搬移：{}；搬移保留其已儲存的進度：請以 `archive relocate {} {} --resume` 繼續",
        Match::Cause},
    {"the new carrier does not hold the recorded content; nothing was changed: copy the archive again from its "
        "carrier to the new one, then relocate again",
        "新載體上的內容與記錄不符；未做任何變更：請從封存的載體重新複製到新載體，再重新搬移"},
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
        "nothing was changed (finish it with `register {} --resume` first, or abandon it with "
        "`archive abandon-registration {}`)",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}；未做任何變更"
        "（請先以 `register {} --resume` 完成該註冊，或以 `archive abandon-registration {}` 放棄它）"},
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
        "(finish that registration with `register {} --resume`, or abandon it with `archive abandon-registration {}` "
        "and import again)",
        "封存 {} 有一個未完成的註冊（根目錄「{}」）正在建立版次 {}；未匯入任何內容（請以 `register {} --resume` "
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
    {"its content hashes do not match its merkle_root", "其內容雜湊與其 Merkle 根雜湊不符"},
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
    {"io must be an object", "設定檔中的 io 必須是 JSON 物件"},
    {"io.rotational_override must give each device serial \"hdd\" or \"ssd\"",
        "設定檔中的 io.rotational_override 必須為每個裝置序號指定 \"hdd\" 或 \"ssd\""},
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
    // The repository that cannot be opened, named with its path (decision 0037).
    {"the configured repository {} is unavailable or is not a VeriArchive repository (the config file {} names it)",
        "設定的儲存庫 {} 無法使用，或不是 VeriArchive 儲存庫（記錄在設定檔 {} 中）"},
    {"VERIARCHIVE_REPOSITORY names the repository {}, which is unavailable or is not a VeriArchive repository",
        "VERIARCHIVE_REPOSITORY 指定的儲存庫 {} 無法使用，或不是 VeriArchive 儲存庫"},
    {"the repository {} is unavailable or is not a VeriArchive repository",
        "儲存庫 {} 無法使用，或不是 VeriArchive 儲存庫"},
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
    // A database that `repo rebuild-index` makes again (decision 0036), as
    // every command and `repo init` name it, and what stops it being made again.
    {"the repository's database {} is missing; make it again from the manifests with `repo rebuild-index`",
        "儲存庫的資料庫 {} 已遺失；請以 `repo rebuild-index` 從清單檔重新建立"},
    {"the repository's database {} is empty; make it again from the manifests with `repo rebuild-index`",
        "儲存庫的資料庫 {} 是空的；請以 `repo rebuild-index` 從清單檔重新建立"},
    {"the repository's database {} is not a database; make it again from the manifests with `repo rebuild-index`",
        "儲存庫的資料庫 {} 不是資料庫檔案；請以 `repo rebuild-index` 從清單檔重新建立"},
    {"the repository's database {} is damaged; make it again from the manifests with `repo rebuild-index`",
        "儲存庫的資料庫 {} 已損毀；請以 `repo rebuild-index` 從清單檔重新建立"},
    {"the repository's database {} was made again, but its rebuild from the manifests did not finish; finish it "
        "with `repo rebuild-index`",
        "儲存庫的資料庫 {} 已重新建立，但從清單檔重建索引的工作沒有完成；請以 `repo rebuild-index` 完成它"},
    {"cannot remove the unfinished new database {}: {}", "無法移除未完成的新資料庫 {}：{}", Match::Cause},
    {"cannot put the new repository database in place: {}: {}", "無法將新的儲存庫資料庫放到原位：{}：{}",
        Match::Cause},
    {"cannot read the repository's database: {}", "無法讀取儲存庫的資料庫：{}", Match::Cause},
    {"the repository's database {} is not a file", "儲存庫的資料庫 {} 不是檔案"},
    {"cannot put the repository's database aside: {}: {}", "無法將儲存庫的資料庫移到一旁：{}：{}", Match::Cause},
    {"unsupported or incomplete repository schema", "不支援或不完整的儲存庫結構描述"},
    // Decision 0032: a repository of an older schema version is upgraded when
    // opened, after a copy of its database is kept beside it; without the
    // copy, it is not upgraded.
    {"cannot upgrade the repository database to schema version {}: {}", "無法將儲存庫資料庫升級到結構描述版本 {}：{}",
        Match::Cause},
    {"cannot keep a copy of the repository database at {} before upgrading it from schema version {} to {}, so the "
        "repository was not upgraded: {}",
        "儲存庫資料庫從結構描述版本 {1} 升級到 {2} 之前，無法先在 {0} 保留它的副本，因此儲存庫沒有升級：{3}",
        Match::Cause},
    {"the repository database was upgraded from schema version {} to {}, which earlier versions of VeriArchive "
        "cannot open; a copy of it from before the upgrade is kept at {}",
        "儲存庫資料庫已從結構描述版本 {} 升級到 {}，較早版本的 VeriArchive 無法再開啟它；"
        "升級前的資料庫副本保存在 {}"},
    // Where each copy of the database kept before an upgrade is, as Settings
    // and `repo info` show it for as long as the copy is in RepositoryRoot,
    // and what it is for.
    {"Copy of the database from before an upgrade: {} (an earlier version of VeriArchive can open only this copy; "
        "you can delete it once you no longer need that version)",
        "升級前的資料庫副本：{}（較早版本的 VeriArchive 只能開啟這份副本；不再需要那個版本時，就可以刪除它）"},
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
    {"not a chunk line", "不是區塊行"},
    {"b3 is not 64 lowercase hex digits", "b3 不是 64 個小寫十六進位數字"},
    {"chunk_size {} is not 0 or a multiple of {} up to {}", "chunk_size {} 不是 0，也不是 {} 的倍數（上限 {}）"},
    // src/core/manifest/manifest_reader.cpp
    {"manifest does not end with a newline", "清單檔未以換行字元結尾"},
    {"empty manifest", "清單檔是空的"},
    {"invalid manifest header: {}", "清單檔標頭無效：{}", Match::Cause},
    {"manifest body does not match its header", "清單檔主體與其標頭不符"},
    {"the manifest changed while it was read", "清單檔在讀取期間被變更"},
    {"invalid manifest entry: {}", "清單檔項目無效：{}", Match::Cause},
    {"manifest entries out of canonical order", "清單檔項目未依標準順序排列"},
    // Chunk lines and the Merkle root (decision 0031): a technical message
    // says Merkle 根雜湊, where the interface says 總碼.
    {"merkle_root is missing or not 64 lowercase hex digits", "merkle_root 缺少或不是 64 個小寫十六進位數字"},
    {"sha256 is not 64 lowercase hex digits", "sha256 不是 64 個小寫十六進位數字"},
    {"counts.chunks is larger than body.lines", "counts.chunks 大於 body.lines"},
    {"chunk line among the manifest entries", "清單檔項目之間出現區塊行"},
    {"manifest entry among the chunk lines", "區塊行之間出現清單檔項目"},
    {"invalid chunk line: {}", "區塊行無效：{}", Match::Cause},
    {"not canonical JSON", "不是標準格式的 JSON"},
    {"chunk {} of {} is missing or does not match the file", "{1} 的區塊 {0} 遺失或與檔案不符"},
    {"chunk {} of {} belongs to no file", "{1} 的區塊 {0} 不屬於任何檔案"},
    {"manifest content hashes do not match its merkle_root", "清單檔的內容雜湊與其 Merkle 根雜湊不符"},
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
    {"chunk hashes of {}: {} given, {} expected", "{} 的區塊雜湊：提供 {} 個，應為 {} 個"},
    // src/core/identity/matcher.cpp
    // What matching noted, in plain words in the warning "identity: ..."
    // (src/core/engine/verify_engine.cpp); --json keeps the codes.
    {"the volume label differs from the one recorded", "磁碟區標籤與記錄中的不同"},
    {"the capacity differs from the one recorded", "容量與記錄中的不同"},
    {"the filesystem type differs from the one recorded", "檔案系統類型與記錄中的不同"},
    {"the device model differs from the one recorded", "裝置型號與記錄中的不同"},
    {"the device was replaced", "裝置已更換"},
    {"the filesystem id cannot be read", "讀不到檔案系統 UUID"},
    // Shown as "identity: the archive is retired": the archive, not its identity.
    {"the archive is retired", "封存已停用"},
    {"the identity file was copied from other media", "身分檔案是從其他媒體複製而來"},
    // src/core/hash/hashers.cpp
    {"cannot initialise SHA-256", "無法初始化 SHA-256"},
    // src/platform/windows/win_util.cpp: what went wrong in plain words, then
    // the call and Windows' number (decision 0017, amended 2026-10-27).
    {"not found ({}, Windows error {})", "找不到（{}，Windows 錯誤 {}）"},
    {"access denied ({}, Windows error {})", "存取被拒（{}，Windows 錯誤 {}）"},
    {"no such drive letter ({}, Windows error {})", "沒有這個磁碟機代號（{}，Windows 錯誤 {}）"},
    {"the media is write-protected ({}, Windows error {})", "媒體已防寫（{}，Windows 錯誤 {}）"},
    {"the device is not ready ({}, Windows error {})", "裝置尚未就緒（{}，Windows 錯誤 {}）"},
    {"the device reported an error ({}, Windows error {})", "裝置回報錯誤（{}，Windows 錯誤 {}）"},
    {"damaged and unreadable ({}, Windows error {})", "已損毀而無法讀取（{}，Windows 錯誤 {}）"},
    {"in use by another program ({}, Windows error {})", "正由其他程式使用中（{}，Windows 錯誤 {}）"},
    {"locked by another program ({}, Windows error {})", "已被其他程式鎖定（{}，Windows 錯誤 {}）"},
    {"the volume is full ({}, Windows error {})", "磁碟區已滿（{}，Windows 錯誤 {}）"},
    {"network path not found ({}, Windows error {})", "找不到網路路徑（{}，Windows 錯誤 {}）"},
    {"already exists ({}, Windows error {})", "已經存在（{}，Windows 錯誤 {}）"},
    {"the name is not valid ({}, Windows error {})", "名稱無效（{}，Windows 錯誤 {}）"},
    {"the name is too long ({}, Windows error {})", "名稱過長（{}，Windows 錯誤 {}）"},
    {"not a folder ({}, Windows error {})", "不是資料夾（{}，Windows 錯誤 {}）"},
    {"the folder is not empty ({}, Windows error {})", "資料夾不是空的（{}，Windows 錯誤 {}）"},
    {"the device is not attached ({}, Windows error {})", "裝置未連接（{}，Windows 錯誤 {}）"},
    {"not permitted for this user ({}, Windows error {})", "此使用者沒有權限（{}，Windows 錯誤 {}）"},
    {"no answer in time ({}, Windows error {})", "未能及時回應（{}，Windows 錯誤 {}）"},
    {"not supported ({}, Windows error {})", "不支援（{}，Windows 錯誤 {}）"},
    {"aborted before it finished ({}, Windows error {})", "完成前即被中止（{}，Windows 錯誤 {}）"},
    {"Windows error {} ({})", "Windows 錯誤 {}（{}）"},
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
    {"Function not implemented", "功能未實作"},
    // On Linux file systems EBADMSG is a checksum failure and EUCLEAN a
    // corrupted file system, and ECANCELED a request the kernel aborted, which
    // nobody cancelled (decision 0034); the literal wording would not say so.
    {"Operation canceled", "完成前即被中止"},
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
