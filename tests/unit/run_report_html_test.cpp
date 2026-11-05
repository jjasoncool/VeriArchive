// The verification report of a recorded run (src/app/run_report_html.*,
// decision 0048). Every archive, run, item, device, computer, hash and time
// below is sample data made up for the test (ARCH-SAMPLE-*, c0ffee00-...
// uuids, SAMPLE-* names); none is of a real archive, device or computer, and
// the repository of read_from_store is a temporary one of its own, as are the
// fake volume (/vol) and the sample archive of refusing_archive_roots, and the
// sample folders, link and archive of refusing_archive_roots_through_a_link.
#include "engine_fixture.h"
#include "test_repository.h"
#include "test_support.h"

#include "app/history.h"
#include "app/local_time.h"
#include "app/run_report_html.h"
#include "common/time.h"
#include "core/engine/candidates.h"
#include "core/engine/carrier_confirmation.h"
#include "core/engine/comparison.h"
#include "core/engine/read_speed.h"
#include "core/identity/volume_json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using va::app::Language;
using va::app::ReportFacts;
using va::app::Text;

const va::Uuid archive = "c0ffee00-0000-4000-8000-000000000021";

// 2026-11-04T12:00:00Z, the sample "now".
const std::chrono::sys_seconds now = std::chrono::sys_days{std::chrono::year{2026} / 11 / 4} + 12h;

std::string ago(std::chrono::seconds before)
{
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now - before);
}

// A sample read of the content of 100 GB: each hundredth 1 GB read in
// `milliseconds_each`.
va::ReadSpeed sample_read(std::uint64_t milliseconds_each)
{
    va::ReadSpeed speed;
    speed.content_bytes = 100'000'000'000;
    speed.devices = {"sample SAMPLE-SERIAL"};
    speed.volume = "c0ffee00-0000-4000-8000-0000000000aa";
    for (std::uint32_t hundredth = 0; hundredth < 100; ++hundredth) {
        speed.parts.push_back({hundredth, 1'000'000'000, milliseconds_each});
    }
    return speed;
}

va::VerifyRunRecord sample_run(std::string id, std::string result, std::chrono::seconds before, nlohmann::json counts)
{
    va::VerifyRunRecord run;
    run.run_id = std::move(id);
    run.archive_uuid = archive;
    run.revision = 1;
    run.mode = "FULL";
    run.started = ago(before + 5min + 37s);
    run.finished = ago(before);
    run.result = std::move(result);
    run.coverage_bytes = 1024 * 1024;
    run.counts_json = counts.dump();
    run.host = "SAMPLE-RECORDED-HOST";
    run.app_version = "0.0.0-sample";
    run.row_hash = "c0ffee00" + run.run_id.substr(run.run_id.size() - 8) + std::string(48, 'a');
    return run;
}

va::VolumeInfo recorded_volume()
{
    va::VolumeInfo volume;
    volume.filesystem_type = "ntfs";
    volume.filesystem_uuid = "c0ffee00c0ffee21";
    volume.volume_label = "SAMPLE-RECORDED-LABEL";
    volume.capacity_bytes = 8001563222016;
    va::PhysicalDeviceInfo device;
    device.model = "SAMPLE-MODEL";
    device.serial = "SAMPLE-SERIAL";
    device.bus = "usb";
    volume.devices = {device};
    return volume;
}

// The facts of a report of `run`, the last of its archive's runs after
// `earlier`, with its evidence, the only archive of a repository whose chain
// was found intact, made at the sample "now".
ReportFacts facts_of(va::VerifyRunRecord run, std::vector<va::VerifyRunRecord> earlier = {})
{
    ReportFacts facts;
    facts.recorded.history = std::move(earlier);
    facts.recorded.history.push_back(run);
    facts.recorded.run = std::move(run);
    facts.recorded.evidence = va::EvidenceRecord{archive, facts.recorded.run.run_id, facts.recorded.run.finished,
        va::volume_to_json_text(recorded_volume()), "MATCH", ""};
    facts.recorded.label = "ARCH-SAMPLE-1";
    facts.recorded.volume_label = "SAMPLE-CURRENT-LABEL";
    facts.runs = facts.recorded.history;
    facts.labels = {{archive, "ARCH-SAMPLE-1"}};
    va::HistoryCheck intact;
    intact.runs = facts.runs.size();
    facts.chain = intact;
    facts.made = {ago(0s), "SAMPLE-MAKING-HOST", "0.0.0-sample"};
    return facts;
}

std::string html_of(const Text& text, const ReportFacts& facts, const va::app::ShownIdentity& shown = {})
{
    const auto html = va::app::run_report_html(text, facts, shown);
    CHECK(html.has_value());
    return html ? *html : std::string{};
}

std::size_t count_of(std::string_view html, std::string_view words)
{
    std::size_t count = 0;
    for (auto at = html.find(words); at != std::string_view::npos; at = html.find(words, at + words.size())) {
        ++count;
    }
    return count;
}

// Whether `first` is in `html`, and before `second`.
bool in_order(std::string_view html, std::string_view first, std::string_view second)
{
    const auto one = html.find(first);
    const auto two = html.find(second);
    return one != std::string_view::npos && two != std::string_view::npos && one < two;
}

va::VerifyItemRecord sample_item(const va::VerifyRunRecord& run, std::string path, std::string status,
    std::string detail = {})
{
    va::VerifyItemRecord item;
    item.run_id = run.run_id;
    item.path.p = std::move(path);
    item.status = std::move(status);
    item.detail = std::move(detail);
    return item;
}

// Every value is escaped: a path, a label and a volume label may hold < & "
// and ', and no name can open an element of its own.
void escaping()
{
    using va::app::html_escaped;
    CHECK(html_escaped("a<b>&\"'c") == "a&lt;b&gt;&amp;&quot;&#39;c");
    CHECK(html_escaped("tab\tline\nend") == "tab\tline\nend");
    CHECK(html_escaped("bell\x07") == "bell�" && html_escaped("del\x7f") == "del�");
    CHECK(html_escaped("bad \xff byte") == "bad � byte");
    // The C1 controls too (U+0080..U+009F), as NEL in a Linux file name;
    // the characters after them stay.
    CHECK(html_escaped("nel\xC2\x85" "end") == "nel\xEF\xBF\xBD" "end" && html_escaped("\xC2\x9F") == "\xEF\xBF\xBD");
    CHECK(html_escaped("\xC2\xA0nbsp \xC2\xA9") == "\xC2\xA0nbsp \xC2\xA9");
    CHECK(html_escaped("中文 😀") == "中文 😀");

    const Text english;
    auto run = sample_run("c0ffee00-0000-4000-9000-000000000201", "FAILED", 1h,
        {{"items", {{"Verified", 2}, {"Modified", 1}}}});
    auto facts = facts_of(run);
    facts.recorded.items = {
        sample_item(run, "SAMPLE/<script>alert(\"x\")</script> & 'q'.txt", "Modified", "content differs")};
    facts.recorded.label = "ARCH-<SAMPLE>&";
    auto volume = recorded_volume();
    volume.volume_label = "SAMPLE-\"VOLUME\"<b>";
    facts.recorded.evidence->volume_json = va::volume_to_json_text(volume);
    const auto html = html_of(english, facts);
    CHECK(!html.contains("<script") && !html.contains("<b>") && !html.contains("ARCH-<SAMPLE>"));
    CHECK(html.contains("SAMPLE/&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt; &amp; &#39;q&#39;.txt"));
    CHECK(html.contains("ARCH-&lt;SAMPLE&gt;&amp;") && html.contains("SAMPLE-&quot;VOLUME&quot;&lt;b&gt;"));
    // Self-contained: no script, nothing loaded from elsewhere.
    CHECK(!html.contains("<link") && !html.contains("src=") && !html.contains("url(") && !html.contains("@import"));
    CHECK(html.starts_with("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">") &&
          html.ends_with("</main>\n</body>\n</html>\n"));
}

// Each result in words, with its mode, beside its tone; an INCOMPLETE run
// has no report.
void each_result()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const nlohmann::json counts{{"items", {{"Verified", 3}}}};
    auto html = html_of(english, facts_of(sample_run("c0ffee00-0000-4000-9000-000000000211", "PASSED", 1h, counts)));
    CHECK(html.contains("<header class=\"banner tone-success\">") &&
          html.contains("</span> FULL VERIFY PASSED</h1>") &&
          html.contains("<p>100% of the registered content was read and matched</p>") &&
          html.contains("<p class=\"dim\">Every entry was verified.</p>"));
    html = html_of(chinese, facts_of(sample_run("c0ffee00-0000-4000-9000-000000000212", "PASSED_WITH_WARNINGS", 1h,
                                counts)));
    CHECK(html.contains("<header class=\"banner tone-warning\">") && html.contains("</span> 完整驗證 通過（有警告）</h1>"));
    auto failed = sample_run("c0ffee00-0000-4000-9000-000000000213", "FAILED", 1h, counts);
    failed.failure_classes = "MediaIdentityMismatch";
    html = html_of(english, facts_of(failed));
    CHECK(html.contains("<header class=\"banner tone-error\">") && html.contains("</span> FULL VERIFY FAILED</h1>") &&
          html.contains("<p>Failure: the media is not the one recorded</p>"));
    // The guarantee (R-02) in every report.
    CHECK(html.contains(va::app::html_escaped(va::app::guarantee)));

    const auto incomplete = sample_run("c0ffee00-0000-4000-9000-000000000214", "INCOMPLETE", 1h, counts);
    const auto refused = va::app::run_report_html(english, facts_of(incomplete));
    CHECK(!refused && refused.error() == va::app::no_report_of_incomplete);
    CHECK(Text(Language::TraditionalChinese)(va::app::no_report_of_incomplete) == "未完成的驗證不能儲存報告。");
}

// Its items as the page orders them, what fails the run first, then what
// warns, then what changes no result, each in the order of its path; all of
// them, where the page lists 200 at a time.
void items_in_order()
{
    const Text english;
    const auto run = sample_run("c0ffee00-0000-4000-9000-000000000221", "FAILED", 1h,
        {{"items", {{"Verified", 10}, {"Modified", 2}, {"Unbaselined", 1}, {"MetadataMismatch", 1}}}});
    auto facts = facts_of(run);
    facts.recorded.items = {sample_item(run, "SAMPLE/A-first.txt", "Unbaselined"),
        sample_item(run, "SAMPLE/B.docx", "Modified", "content differs"),
        sample_item(run, "SAMPLE/C.pdf", "Modified", "content differs"),
        sample_item(run, "SAMPLE/D.xlsx", "MetadataMismatch", "mtime"),
        sample_item(run, "SAMPLE-mounted", "SkippedMountPoint", va::mount_point_outside_archive)};
    auto html = html_of(english, facts);
    CHECK(in_order(html, "SAMPLE/B.docx", "SAMPLE/C.pdf") && in_order(html, "SAMPLE/C.pdf", "SAMPLE/A-first.txt") &&
          in_order(html, "SAMPLE/A-first.txt", "SAMPLE/D.xlsx") && in_order(html, "SAMPLE/D.xlsx", "SAMPLE-mounted"));
    CHECK(html.contains("<span class=\"pill tone-error\">Modified</span></td><td class=\"code\">SAMPLE/B.docx</td>"
                        "<td>content differs</td>"));
    CHECK(html.contains("<span class=\"pill tone-warning\">MetadataMismatch</span></td><td class=\"code\">"
                        "SAMPLE/D.xlsx</td><td>modification time</td>"));
    CHECK(html.contains("<span class=\"pill tone-neutral\">SkippedMountPoint</span>"));
    // A run that FAILED lists them first, before what it read.
    CHECK(in_order(html, "<h2>Items (5)</h2>", "<div class=\"metrics\">"));
    CHECK(html.contains("<th scope=\"col\">Status</th><th scope=\"col\">Path</th><th scope=\"col\">Detail</th>"));

    // A report is a record: every item, however many.
    facts.recorded.items.clear();
    for (int index = 0; index < 250; ++index) {
        facts.recorded.items.push_back(sample_item(run, std::format("SAMPLE/file-{:03}.bin", index), "Modified"));
    }
    html = html_of(english, facts);
    CHECK(count_of(html, "<tr><td><span class=\"pill tone-error\">Modified</span>") == 250 &&
          html.contains("SAMPLE/file-249.bin") && html.contains("<h2>Items (250)</h2>"));

    // A run that PASSED lists them after its read speed.
    const auto passed = sample_run("c0ffee00-0000-4000-9000-000000000222", "PASSED", 1h,
        {{"items", {{"Verified", 6}}}, {"read_speed", va::read_speed_json(sample_read(10'000))}});
    html = html_of(english, facts_of(passed));
    CHECK(in_order(html, "<section class=\"speed\">", "<section class=\"items\">"));
}

// The archive's runs, newest first, the reported one marked in words; the
// note of a relocation's run and the device alerts of each.
void run_list()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto first = sample_run("c0ffee00-0000-4000-9000-000000000231", "PASSED", 24h * 30,
        {{"items", {{"Verified", 3}}}, {"relocated_from_revision", 1}});
    const auto reported = sample_run("c0ffee00-0000-4000-9000-000000000232", "FAILED", 24h * 10,
        {{"items", {{"Verified", 3}}}});
    auto later = sample_run("c0ffee00-0000-4000-9000-000000000233", "INCOMPLETE", 24h,
        {{"items", {{"Verified", 1}}}});
    later.revision = 2;
    auto facts = facts_of(reported, {first});
    facts.recorded.history.push_back(later);
    facts.runs = facts.recorded.history;
    const auto html = html_of(english, facts);
    const auto runs = html.substr(html.find("<section class=\"runs\">"));
    CHECK(runs.starts_with("<section class=\"runs\">\n<h2>Runs of this archive (3)</h2>"));
    CHECK(in_order(runs, later.run_id, reported.run_id) && in_order(runs, reported.run_id, first.run_id));
    CHECK(count_of(html, "<tr class=\"this-run\">") == 1 && count_of(html, "This run</span>") == 1);
    const auto marked = runs.substr(runs.find("<tr class=\"this-run\">"));
    CHECK(marked.substr(0, marked.find("</tr>")).contains(reported.run_id) &&
          marked.contains("FULL VERIFY FAILED</span><span class=\"this\">This run</span>"));
    CHECK(runs.contains("<span class=\"pill tone-neutral\">FULL VERIFY INCOMPLETE</span>") &&
          runs.contains("<td class=\"number\">2</td>") && runs.contains("<span class=\"note\">relocation from r1</span>"));
    CHECK(runs.contains(va::app::html_escaped(va::app::local_time(first.finished, english))));
    const auto zh = html_of(chinese, facts);
    CHECK(zh.contains("<h2>此封存的執行（3）</h2>") && zh.contains("這次執行</span>") &&
          zh.contains("<th scope=\"col\" class=\"when\">完成時間<span class=\"sub\">執行</span></th>"
                      "<th scope=\"col\">結果</th><th scope=\"col\">版次</th><th scope=\"col\">備註</th>"));
    // Each run's id under when it ended, never broken.
    CHECK(runs.contains("<td class=\"when\">" + va::app::html_escaped(va::app::local_time(later.finished, english)) +
                        "<span class=\"sub code\">" + later.run_id + "</span></td>"));
}

// The chain when the report was made, as History says it, with this run's
// hash in it: intact, broken at its first run that does not link, or not
// checked.
void chain()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto first = sample_run("c0ffee00-0000-4000-9000-000000000241", "PASSED", 24h * 3,
        {{"items", {{"Verified", 3}}}});
    const auto run = sample_run("c0ffee00-0000-4000-9000-000000000242", "PASSED", 1h, {{"items", {{"Verified", 3}}}});
    auto facts = facts_of(run, {first});
    auto html = html_of(english, facts);
    CHECK(html.contains("<h2>History chain when this report was made</h2>\n<p class=\"tone-success\">"
                        "<span class=\"mark\" aria-hidden=\"true\">✓</span> The history of every archive is "
                        "intact: 2 verification runs, hash chain verified.</p>"));
    CHECK(html.contains(va::app::html_escaped(va::app::history_chain_limits)));
    CHECK(html.contains("<dt>This run&#39;s hash in the history chain (row_hash)</dt><dd class=\"code\">" +
                        run.row_hash + "</dd>"));

    va::HistoryCheck broken;
    broken.runs = 2;
    broken.first_broken_run = first.run_id;
    broken.problem = "row was edited (row_hash does not match its contents)";
    facts.chain = broken;
    html = html_of(english, facts);
    CHECK(html.contains("<p class=\"tone-error\"><span class=\"mark\" aria-hidden=\"true\">✕</span> History BROKEN "
                        "at run " +
                        first.run_id + " (ARCH-SAMPLE-1 · " +
                        va::app::html_escaped(va::app::local_time(first.finished, english)) +
                        "): row was edited (row_hash does not match its contents)</p>"));
    html = html_of(chinese, facts);
    CHECK(html.contains("<h2>製作此報告時的歷程鏈</h2>") && html.contains("歷程於執行 " + first.run_id + "（ARCH-SAMPLE-1・") &&
          html.contains("處斷裂：此筆紀錄已被修改（row_hash 與其內容不符）"));

    facts.chain = std::unexpected(va::IoError{va::IoErrorKind::Other, 0, "sample failure"});
    html = html_of(english, facts);
    CHECK(html.contains("Cannot check the history: sample failure</p>") &&
          !html.contains(va::app::html_escaped(va::app::history_chain_limits)));
}

// Decision 0047: a run that went on past a changed enclosure says how it was
// confirmed, the enclosure's serial as the page labels it, and the device's
// own identity that the confirmation read behind it, which the window's page
// does not show.
void confirmed_run()
{
    const Text english;
    va::CarrierConfirmation confirmation;
    confirmation.how = va::CarrierConfirmation::How::DeviceSerial;
    confirmation.device = va::DeviceIdentity{"SAMPLE-OWN-MODEL", "SAMPLE-OWN-SERIAL", "SAMPLE-OWN-WWN"};
    confirmation.enclosure_recorded = "SAMPLE-ENCLOSURE-OLD";
    confirmation.enclosure_now = "SAMPLE-ENCLOSURE-NEW";
    const auto run = sample_run("c0ffee00-0000-4000-9000-000000000251", "PASSED", 1h,
        {{"items", {{"Verified", 3}}}, {va::carrier_confirmed_member, va::carrier_confirmation_json(confirmation, 1)}});
    auto facts = facts_of(run);
    auto volume = recorded_volume();
    volume.devices.front().serial = "SAMPLE-ENCLOSURE-NEW";
    volume.devices.front().enclosure_hides_identity = true;
    facts.recorded.evidence->volume_json = va::volume_to_json_text(volume);
    const auto html = html_of(english, facts);
    CHECK(html.contains("<p>enclosure changed, same device by its serial</p>"));
    CHECK(html.contains("<dt>Media identity</dt><dd>MIGRATED: the enclosure differs from the one recorded (the "
                        "device&#39;s serial matches)</dd>"));
    CHECK(html.contains("<dt>Enclosure serial</dt><dd class=\"code\">SAMPLE-ENCLOSURE-NEW</dd>"));
    CHECK(html.contains("<h3>The device&#39;s own identity, read behind its enclosure</h3>") &&
          html.contains("<dt>Device&#39;s own serial</dt><dd class=\"code\">SAMPLE-OWN-SERIAL</dd>") &&
          html.contains("<dt>WWN</dt><dd class=\"code\">SAMPLE-OWN-WWN</dd>") &&
          html.contains("<span class=\"note\">enclosure changed, same device by its serial</span>"));
    // The window's page shows the same identity, but not the read.
    const auto page = va::app::result_page(english, facts.recorded);
    CHECK(page.confirmed_device.size() == 3 &&
          std::ranges::none_of(page.identity, [](const va::app::Field& field) {
              return field.value.contains("SAMPLE-OWN");
          }));
    // As a view shows it: in the screenshot mode the sample device and
    // computer in place of every recorded one.
    va::app::ShownIdentity shown;
    shown.devices = [](const std::vector<va::PhysicalDeviceInfo>& read) {
        std::vector<va::PhysicalDeviceInfo> devices(read.size());
        for (auto& device : devices) {
            device.model = "SHOWN-MODEL";
            device.serial = "SHOWN-SERIAL";
        }
        return devices;
    };
    shown.volume = [](const va::VolumeInfo&) {
        va::VolumeInfo volume;
        volume.volume_label = "SHOWN-VOLUME";
        return volume;
    };
    shown.host = [](const std::string&) { return std::string("SHOWN-HOST"); };
    const auto sample = html_of(english, facts, shown);
    CHECK(!sample.contains("SAMPLE-OWN-SERIAL") && !sample.contains("SAMPLE-ENCLOSURE-NEW") &&
          !sample.contains("SAMPLE-RECORDED-HOST") && !sample.contains("SAMPLE-MAKING-HOST") &&
          !sample.contains("SAMPLE-RECORDED-LABEL"));
    CHECK(sample.contains("<dt>Device&#39;s own serial</dt><dd class=\"code\">SHOWN-SERIAL</dd>") &&
          sample.contains("<dt>Computer</dt><dd>SHOWN-HOST</dd>") && sample.contains(" on the computer SHOWN-HOST ") &&
          sample.contains("ARCH-SAMPLE-1 · SHOWN-VOLUME · "));
}

// A run without media evidence: its archive's current carrier names it, and
// the media identity is not known.
void without_evidence()
{
    const Text english;
    auto facts = facts_of(sample_run("c0ffee00-0000-4000-9000-000000000261", "PASSED", 1h, {{"items", {{"Verified", 3}}}}));
    facts.recorded.evidence.reset();
    const auto html = html_of(english, facts);
    CHECK(html.contains("<p class=\"archive\">ARCH-SAMPLE-1 · SAMPLE-CURRENT-LABEL · " +
                        va::app::html_escaped(va::app::local_time(facts.recorded.run.finished, english)) + "</p>"));
    CHECK(html.contains("<dt>Media identity</dt><dd>not known</dd>") && !html.contains("<dt>Volume</dt>"));
    CHECK(html.contains("<dt>Archive</dt><dd>ARCH-SAMPLE-1</dd>\n<dt>Archive UUID</dt><dd class=\"code\">" + archive +
                        "</dd>"));
}

// The read speed chart as inline SVG where the page draws one, with its axis
// words; else the page's reason.
void speed()
{
    const Text english;
    const auto baseline_read = sample_read(8'000);
    const auto before = sample_run("c0ffee00-0000-4000-9000-000000000271", "PASSED", 24h * 90,
        {{"items", {{"Verified", 3}}}, {"read_speed", va::read_speed_json(baseline_read)}});
    const auto own = sample_read(10'000);
    auto compared = va::compare_read_speed(own, baseline_read);
    CHECK(compared.has_value());
    if (!compared) {
        return;
    }
    compared->baseline_run = before.run_id;
    compared->baseline_finished = before.finished;
    compared->slower_parts = {{20, 29, 60}};
    const auto run = sample_run("c0ffee00-0000-4000-9000-000000000272", "PASSED", 1h,
        {{"items", {{"Verified", 3}}}, {"read_speed", va::read_speed_json(own)},
            {"read_compared", va::speed_comparison_json(*compared)}});
    auto html = html_of(english, facts_of(run, {before}));
    CHECK(html.contains("<svg class=\"chart\" viewBox=\"0 0 960 240\" role=\"img\" aria-label=\"Read speed by "
                        "position in the content\""));
    CHECK(html.contains("<i class=\"swatch now\"></i>This verification</span>") &&
          html.contains("The one compared with (dashed)") && html.contains("Markedly slower") &&
          html.contains("<span class=\"unit\">Vertical axis: MiB/s</span>"));
    CHECK(count_of(html, "<path class=\"now-line\"") == 1 && count_of(html, "<path class=\"before\"") == 1 &&
          count_of(html, "<rect class=\"slower\"") == 1);
    // The axis from 0 to 150 in steps of 50, as the page's.
    CHECK(html.contains(">0</text>") && html.contains(">50</text>") && html.contains(">150</text>") &&
          html.contains(">100%</text>"));
    CHECK(html.contains("<p class=\"dim\">Read speed: 95.4 MiB/s; the last FULL verification"));
    // An old run, without its read speed: the version did not record it.
    html = html_of(english, facts_of(sample_run("c0ffee00-0000-4000-9000-000000000273", "PASSED", 1h,
                                {{"items", {{"Verified", 3}}}})));
    CHECK(!html.contains("<svg") &&
          html.contains("<p>The version that ran this verification did not record read speed yet.</p>"));
}

// The language of the window or of --lang, set on the page.
void languages()
{
    const Text chinese(Language::TraditionalChinese);
    const auto facts =
        facts_of(sample_run("c0ffee00-0000-4000-9000-000000000281", "PASSED", 1h, {{"items", {{"Verified", 3}}}}));
    const auto html = html_of(chinese, facts);
    CHECK(html.starts_with("<!doctype html>\n<html lang=\"zh-TW\">"));
    CHECK(html.contains("<title>驗證報告・ARCH-SAMPLE-1・") && html.contains("<p class=\"kind\">VeriArchive・驗證報告</p>") &&
          html.contains("</span> 完整驗證 通過</h1>") && html.contains("<h2>裝置與身分</h2>") &&
          html.contains("<h2>技術細節</h2>") && html.contains("<div class=\"name\">已讀取且相符</div>"));
    CHECK(html.contains("<p>由電腦 SAMPLE-MAKING-HOST 上的 VeriArchive 0.0.0-sample 製作於 " +
                        va::app::html_escaped(va::app::local_time(facts.made.at, chinese)) + "。</p>"));
    // How to check the file: a new report compared with it, in what the two
    // say of the run as well as in the hash and the chain's state, since
    // row_hash covers neither the run's items nor the file.
    CHECK(html.contains("<p>儲存庫才是紀錄：此檔案說的是製作當時儲存庫所記錄的內容。此檔案本身沒有簽章，無法顯示事後是否被"
                        "修改；若要核對，請從儲存庫再製作一份這次執行的報告（在其結果頁按「儲存報告…」，或用 "
                        "veriarchive-cli report），比對兩份報告中這次執行的內容、這次執行在歷程鏈中的雜湊與歷程鏈的狀態。"
                        "</p>\n</footer>"));
    const auto english = html_of(Text(), facts);
    CHECK(english.contains("<p>Made " + va::app::html_escaped(va::app::local_time(facts.made.at, Text())) +
                           " on the computer SAMPLE-MAKING-HOST by VeriArchive 0.0.0-sample.</p>") &&
          english.contains("<p>The repository is the record: this file says what the repository recorded when the file "
                           "was made. The file itself is not signed, so it cannot show a change made to it afterwards; "
                           "to check it, make a new report of this run from the repository (Save report... on its "
                           "result page, or veriarchive-cli report) and compare the two: what they say of this run, "
                           "this run&#39;s hash in the history chain and the chain&#39;s state.</p>\n</footer>"));
}

// The same records and time of making give the same bytes.
void determinism()
{
    const Text english;
    const auto run = sample_run("c0ffee00-0000-4000-9000-000000000291", "FAILED", 1h,
        {{"items", {{"Verified", 3}, {"Modified", 1}}}, {"read_speed", va::read_speed_json(sample_read(10'000))}});
    auto facts = facts_of(run);
    facts.recorded.items = {sample_item(run, "SAMPLE/a.txt", "Modified", "content differs")};
    const auto once = html_of(english, facts);
    CHECK(!once.empty() && once == html_of(english, facts));
    auto later = facts;
    later.made.at = ago(-1h);
    CHECK(html_of(english, later) != once);
}

// The name proposed for the file: the label, the local date the run ended on
// and its result in the language, nothing a file name cannot hold.
void file_name()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    auto facts = facts_of(sample_run("c0ffee00-0000-4000-9000-0000000002a1", "PASSED", 1h, {{"items", {{"Verified", 3}}}}));
    const auto day = va::app::local_date(*va::parse_timestamp(facts.recorded.run.finished));
    const auto date = std::format("{:04}-{:02}-{:02}", static_cast<int>(day.year()), static_cast<unsigned>(day.month()),
        static_cast<unsigned>(day.day()));
    CHECK(va::app::report_file_name(english, facts.recorded) == "ARCH-SAMPLE-1 verification report " + date +
                                                                    " PASSED.html");
    facts.recorded.run.result = "PASSED_WITH_WARNINGS";
    CHECK(va::app::report_file_name(chinese, facts.recorded) == "ARCH-SAMPLE-1 驗證報告 " + date + " 通過（有警告）.html");
    facts.recorded.label = "SAMPLE/a\\b:c*d?e\"f<g>h|i\x01.";
    CHECK(va::app::report_file_name(english, facts.recorded) ==
          "SAMPLE_a_b_c_d_e_f_g_h_i_. verification report " + date + " PASSED_WITH_WARNINGS.html");
    // No space or dot before ".html", which Windows would drop.
    facts.recorded.run.finished.clear();
    facts.recorded.run.result = ".";
    CHECK(va::app::report_file_name(english, facts.recorded) == "SAMPLE_a_b_c_d_e_f_g_h_i_. verification report.html");
}

// What a report reads from a repository: the run with its archive named as
// the views name it, every run, and the chain checked as it is now.
void read_from_store()
{
    test::TestRepository repository;
    auto store = repository.store();
    const auto& repository_uuid = repository.context.repository_uuid;
    CHECK(store.insert_archive({archive, "ARCH-SAMPLE-1", ago(24h * 40), 1, "REGISTERED", "", ""}));
    // Another archive with the same label (R-88): named with its uuid.
    const va::Uuid other = "c0ffee00-0000-4000-8000-000000000022";
    CHECK(store.insert_archive({other, "ARCH-SAMPLE-1", ago(24h * 40), 1, "REGISTERED", "", ""}));
    auto first = sample_run("c0ffee00-0000-4000-9000-0000000002b1", "PASSED", 24h * 2, {{"items", {{"Verified", 3}}}});
    first.archive_uuid = other;
    const auto run = sample_run("c0ffee00-0000-4000-9000-0000000002b2", "FAILED", 1h,
        {{"items", {{"Verified", 2}, {"Modified", 1}}}});
    CHECK(store.append_verify_run(first, repository_uuid));
    CHECK(store.append_verify_run(run, repository_uuid));
    CHECK(store.insert_verify_items({sample_item(run, "SAMPLE/a.txt", "Modified", "content differs")}));
    CHECK(store.insert_evidence({archive, run.run_id, run.finished, va::volume_to_json_text(recorded_volume()), "MATCH", ""}));

    auto read = va::app::read_report_facts(store, repository_uuid, run.run_id);
    CHECK(read && *read);
    if (!read || !*read) {
        return;
    }
    const auto& facts = **read;
    // No other run where the chain is intact.
    CHECK(facts.recorded.label == "ARCH-SAMPLE-1 (c0ffee00)" && facts.recorded.items.size() == 1 &&
          facts.recorded.history.size() == 1 && facts.recorded.evidence && facts.runs.empty() &&
          !facts.recorded.run.row_hash.empty() && facts.labels.size() == 2);
    CHECK(facts.chain && facts.chain->runs == 2 && !facts.chain->first_broken_run);
    const auto html = html_of(Text(), facts);
    CHECK(html.contains("intact: 2 verification runs") && html.contains(facts.recorded.run.row_hash));

    repository.execute("UPDATE verify_runs SET result = 'FAILED' WHERE run_id = 'c0ffee00-0000-4000-9000-0000000002b1'");
    read = va::app::read_report_facts(store, repository_uuid, run.run_id);
    CHECK(read && *read && (*read)->chain && (*read)->chain->first_broken_run == first.run_id);
    // The run it breaks at, of the other archive, by which the report names it.
    CHECK(read && *read && (*read)->runs.size() == 1 && (*read)->runs.front().run_id == first.run_id);
    if (read && *read) {
        CHECK(html_of(Text(), **read).contains("History BROKEN at run " + first.run_id + " (ARCH-SAMPLE-1 (c0ffee00) · "));
    }
    const auto unknown = va::app::read_report_facts(store, repository_uuid, "c0ffee00-0000-4000-9000-0000000002ff");
    CHECK(unknown && !*unknown);
}

// What `veriarchive-cli report` says when it refuses (src/cli/report_commands.cpp
// writes these sentences, which its errors translate), in zh-TW: the e2e test
// that runs it checks the unknown run and the file there already, and no run
// of it is INCOMPLETE.
void command_line_refusals()
{
    const Text chinese(Language::TraditionalChinese);
    CHECK(chinese("run c0ffee00-0000-4000-9000-0000000002c1 did not finish (INCOMPLETE); a report is made only of a "
                  "run that reached a result") ==
          "執行 c0ffee00-0000-4000-9000-0000000002c1 未得出結果（未完成）；只有得出結果的執行才能製作驗證報告");
    CHECK(chinese("no run c0ffee00-0000-4000-9000-0000000002c2") == "找不到執行 c0ffee00-0000-4000-9000-0000000002c2");
    CHECK(chinese("SAMPLE/report.html exists already; a report never replaces a file, so name another one") ==
          "SAMPLE/report.html 已經存在；報告不會取代既有的檔案，請指定另一個檔名");
    CHECK(chinese("usage: report <run_id> --out FILE.html") == "用法：report <run_id> --out FILE.html");
    // The refusal of a folder in an archive's root, which the command makes
    // in English (report_refusal) and its error translates.
    const std::vector<va::app::AttachedArchiveRoot> roots{
        {std::filesystem::path("/media/SAMPLE-1/photos"), "ARCH-SAMPLE-1"}};
    const std::filesystem::path folder("/media/SAMPLE-1/photos/reports");
    const auto refused = va::app::report_refusal(Text(), folder, roots);
    CHECK(refused && chinese(*refused) == va::app::report_refusal(chinese, folder, roots));
}

// Never into the root of an archive attached now, nor a folder below it: the
// file would be an entry that the archive's next FULL verification reports as
// added. Sample data: a sample archive (ARCH-0001) of a few sample files at
// /vol/photos on the fake volume /vol, in a temporary repository.
void refusing_archive_roots()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const std::vector<va::app::AttachedArchiveRoot> roots{
        {std::filesystem::path("/media/SAMPLE-1/photos"), "ARCH-SAMPLE-1"}};
    CHECK(!va::app::report_refusal(english, "/media/SAMPLE-1", roots));
    CHECK(!va::app::report_refusal(english, "/media/SAMPLE-1/photos-reports", roots));
    CHECK(!va::app::report_refusal(english, "/media/SAMPLE-1/photos", {}));
    const auto inside = va::app::report_refusal(english, "/media/SAMPLE-1/photos/reports", roots);
    CHECK(inside && *inside == va::path_to_utf8(std::filesystem::path("/media/SAMPLE-1/photos/reports")) +
                                   " lies in the root of ARCH-SAMPLE-1 (" +
                                   va::path_to_utf8(std::filesystem::path("/media/SAMPLE-1/photos")) +
                                   "): a report saved there would add a file that its next FULL verification "
                                   "reports as added. Choose a folder outside every archive.");
    const auto zh = va::app::report_refusal(chinese, "/media/SAMPLE-1/photos", roots);
    CHECK(zh && zh->contains(" 位於 ARCH-SAMPLE-1 的根目錄（") && zh->ends_with("請選擇不在任何封存中的資料夾。"));

    // As a read of the folder's volume finds them now.
    test::EngineFixture fixture;
    fixture.archive.add_file("photos/a.txt", "sample");
    fixture.archive.add_file("photos/b.txt", "sample too");
    fixture.archive.add_file("elsewhere.txt", "sample, in no archive");
    const auto registered = fixture.register_root(std::filesystem::path("/vol/photos"));
    CHECK(registered.has_value());
    const auto holding = [&](const std::filesystem::path& folder) {
        return va::app::roots_holding(
            fixture.store, *fixture.repository.files, fixture.media, fixture.repository.context.repo, folder);
    };
    const auto below = holding("/vol/photos/reports");
    CHECK(below.size() == 1 && !below.empty() && below.front().label == "ARCH-0001" &&
          va::path_within(below.front().root, "/vol/photos/reports"));
    CHECK(holding("/vol/photos").size() == 1 && holding("/vol").empty() && holding("/vol/photos-reports").empty());
    CHECK(va::app::report_refusal(english, "/vol/photos/reports", below).has_value());
    CHECK(va::app::report_refusal_in_root(english, "/vol/photos/reports", below.front()) ==
          va::app::report_refusal(english, "/vol/photos/reports", below));
    // A volume that cannot be read holds nothing that can be told.
    fixture.media.error = va::IoError{va::IoErrorKind::Other, 0, "sample failure"};
    CHECK(holding("/vol/photos").empty());
}

// A folder written through a link to an archive's root, which names a folder
// of that root by another path, lies in that root as the file system names
// the two (decision 0029, point 6): a junction on Windows, a symbolic link on
// Linux. Sample data: sample folders in a temporary folder of their own, which
// a fake volume has as its mount point, and a sample archive (ARCH-0001) of
// one sample file registered at its folder photos, in a temporary repository.
void refusing_archive_roots_through_a_link()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() / ("va-SAMPLE-report-root-" + std::to_string(stamp));
    std::filesystem::create_directories(base / "photos" / "reports");
    std::filesystem::create_directories(base / "elsewhere");
    const auto link = base / "link-to-photos-SAMPLE";
#ifdef _WIN32
    const auto command = "mklink /J \"" + va::path_to_utf8(link) + "\" \"" + va::path_to_utf8(base / "photos") +
                         "\" >NUL";
    const bool linked = std::system(command.c_str()) == 0;
#else
    std::error_code link_error;
    std::filesystem::create_directory_symlink(base / "photos", link, link_error);
    const bool linked = !link_error;
#endif
    CHECK(linked);
    if (linked) {
        test::EngineFixture fixture(base);
        fixture.archive.add_file("photos/a.txt", "sample");
        CHECK(fixture.register_root(base / "photos").has_value());
        const auto holding = [&](const std::filesystem::path& folder) {
            return va::app::roots_holding(
                fixture.store, *fixture.repository.files, fixture.media, fixture.repository.context.repo, folder);
        };
        // Once, as the volume's mount point names its root, although the
        // fake volume is found again for the folder as named.
        const auto through = holding(link / "reports");
        CHECK(through.size() == 1 && !through.empty() && through.front().label == "ARCH-0001" &&
              through.front().root == base / "photos");
        CHECK(holding(link).size() == 1 && holding(base / "elsewhere").empty() && holding(base).empty());
        // Not as their paths are written, so in the words of the root found.
        CHECK(!through.empty() && !va::app::report_refusal(Text(), link / "reports", through));
        if (!through.empty()) {
            CHECK(va::app::report_refusal_in_root(Text(), link / "reports", through.front())
                      .starts_with(va::path_to_utf8(link / "reports") + " lies in the root of ARCH-0001 (" +
                                   va::path_to_utf8(base / "photos") + "): "));
        }
    }
    std::error_code error;
    std::filesystem::remove(link, error);
    std::filesystem::remove_all(base, error);
}

} // namespace

void run_report_html_tests()
{
    command_line_refusals();
    refusing_archive_roots();
    refusing_archive_roots_through_a_link();
    escaping();
    each_result();
    items_in_order();
    run_list();
    chain();
    confirmed_run();
    without_evidence();
    speed();
    languages();
    determinism();
    file_name();
    read_from_store();
}
