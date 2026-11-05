#include "app/run_report_html.h"

#include "app/device_alerts.h"
#include "app/history.h"
#include "app/local_time.h"
#include "common/time.h"
#include "common/utf8.h"
#include "core/engine/archive_status.h"
#include "core/engine/candidates.h"
#include "core/engine/device_alerts.h"

#include <format>
#include <utility>

namespace va::app {
namespace {

// The report's style: the screen in the light or the dark style the system
// asks for, and on paper light, with no background needed to read it, the
// header of a long table repeated on each page. The tones also have their
// words (and a mark) beside them: never colour alone.
constexpr std::string_view style = R"css(
:root {
  color-scheme: light dark;
  --ground: #ffffff; --ink: #1e1e22; --dim: #5c5c66; --line: #d9d9de; --card: #f5f5f7;
  --success: #1a7337; --success-ground: #e4f4e8; --warning: #8a5300; --warning-ground: #fbefd8;
  --error: #b3261e; --error-ground: #fbe7e5; --neutral: #4a4a52; --neutral-ground: #ececef;
  --accent: #1d5fbf;
}
@media (prefers-color-scheme: dark) {
  :root {
    --ground: #1d1d20; --ink: #ebebef; --dim: #a9a9b3; --line: #3e3e45; --card: #28282c;
    --success: #7fd394; --success-ground: #1d3626; --warning: #f2c463; --warning-ground: #3a2f17;
    --error: #ff9b91; --error-ground: #42201d; --neutral: #c8c8d0; --neutral-ground: #323237;
    --accent: #8bb4f5;
  }
}
* { box-sizing: border-box; }
html { background: var(--ground); }
body {
  margin: 0; background: var(--ground); color: var(--ink);
  font: 15px/1.5 system-ui, -apple-system, "Segoe UI", "Noto Sans", "Noto Sans TC", "Microsoft JhengHei",
    "PingFang TC", sans-serif;
}
main { max-width: 60rem; margin: 0 auto; padding: 2rem 1.25rem 3rem; }
h1, h2, h3 { line-height: 1.25; }
h1 { font-size: 1.6rem; margin: 0.15rem 0 0.35rem; }
h2 { font-size: 1.15rem; margin: 2rem 0 0.6rem; padding-bottom: 0.25rem; border-bottom: 1px solid var(--line); }
h3 { font-size: 1rem; margin: 1.2rem 0 0.4rem; }
p { margin: 0.3rem 0; }
.dim { color: var(--dim); }
.code { font-family: ui-monospace, "Cascadia Mono", Consolas, "DejaVu Sans Mono", "Liberation Mono", monospace;
  font-size: 0.9em; overflow-wrap: anywhere; word-break: break-all; }
.kind { color: var(--dim); font-size: 0.85rem; letter-spacing: 0.02em; margin: 0; }
.banner { border: 2px solid var(--neutral); border-radius: 10px; padding: 1rem 1.25rem; background: var(--neutral-ground); }
.banner.tone-success { border-color: var(--success); background: var(--success-ground); }
.banner.tone-warning { border-color: var(--warning); background: var(--warning-ground); }
.banner.tone-error { border-color: var(--error); background: var(--error-ground); }
.banner h1 .mark { display: inline-block; min-width: 1.2em; }
.tone-success .mark, .tone-success > h1, .pill.tone-success, p.tone-success { color: var(--success); }
.tone-warning .mark, .tone-warning > h1, .pill.tone-warning, p.tone-warning { color: var(--warning); }
.tone-error .mark, .tone-error > h1, .pill.tone-error, p.tone-error { color: var(--error); }
.banner .archive { font-variant-numeric: tabular-nums; }
.pill { display: inline-block; padding: 0 0.5em; border: 1px solid currentColor; border-radius: 999px;
  font-size: 0.85em; white-space: nowrap; color: var(--neutral); }
.this { display: inline-block; margin-left: 0.4em; font-size: 0.85em; font-weight: 700; }
dl.fields { display: grid; grid-template-columns: minmax(9rem, max-content) 1fr; gap: 0.25rem 1.25rem; margin: 0.5rem 0; }
dl.fields dt { color: var(--dim); }
dl.fields dd { margin: 0; overflow-wrap: anywhere; }
ul.alerts { list-style: none; padding: 0; margin: 1rem 0 0; }
ul.alerts li { border-left: 4px solid var(--error); padding: 0.4rem 0.75rem; margin: 0.4rem 0;
  background: var(--error-ground); }
.metrics { display: grid; grid-template-columns: repeat(auto-fit, minmax(10rem, 1fr)); gap: 0.6rem; margin: 1.25rem 0 0; }
.metric { border: 1px solid var(--line); border-radius: 8px; padding: 0.6rem 0.8rem; background: var(--card); }
.metric .value { font-size: 1.25rem; font-weight: 600; font-variant-numeric: tabular-nums; }
.metric .name { color: var(--dim); font-size: 0.85rem; }
table { border-collapse: collapse; width: 100%; font-size: 0.92em; }
th, td { text-align: left; vertical-align: top; padding: 0.35rem 0.6rem 0.35rem 0; border-bottom: 1px solid var(--line); }
th { color: var(--dim); font-weight: 600; white-space: nowrap; }
td.number { font-variant-numeric: tabular-nums; }
th.when, td.when { white-space: nowrap; }
.when .sub { display: block; font-size: 0.8em; font-weight: 400; color: var(--dim); }
.when .sub.code { word-break: normal; overflow-wrap: normal; }
tr.this-run td { font-weight: 600; }
td .note { display: block; }
.legend { display: flex; flex-wrap: wrap; gap: 0.4rem 1.2rem; align-items: center; color: var(--dim); font-size: 0.85rem; }
.legend .unit { margin-left: auto; }
.swatch { display: inline-block; width: 1.4em; height: 0; vertical-align: middle; margin-right: 0.4em; }
.swatch.now { border-top: 3px solid var(--accent); }
.swatch.before { border-top: 2px dashed var(--dim); }
.swatch.slower { height: 0.8em; border: 1px solid var(--error); background: var(--error-ground); }
svg.chart { display: block; width: 100%; height: auto; margin: 0.5rem 0; }
svg.chart .grid { stroke: var(--line); stroke-width: 1; }
svg.chart .tick { fill: var(--dim); font-size: 12px; font-family: inherit; }
svg.chart .now-area { fill: var(--accent); fill-opacity: 0.18; stroke: none; }
svg.chart .now-line { fill: none; stroke: var(--accent); stroke-width: 1.6; }
svg.chart .before { fill: none; stroke: var(--dim); stroke-width: 1.3; stroke-dasharray: 4 3; }
svg.chart .slower { fill: var(--error); fill-opacity: 0.16; stroke: var(--error); stroke-opacity: 0.5; stroke-width: 0.8; }
footer { margin-top: 2.5rem; padding-top: 0.75rem; border-top: 1px solid var(--line); color: var(--dim); font-size: 0.88rem; }
@media print {
  :root {
    color-scheme: light;
    --ground: #ffffff; --ink: #000000; --dim: #444448; --line: #b8b8be; --card: #ffffff;
    --success: #14592a; --success-ground: #ffffff; --warning: #6b4000; --warning-ground: #ffffff;
    --error: #8f1d16; --error-ground: #ffffff; --neutral: #333338; --neutral-ground: #ffffff;
    --accent: #1a4f9e;
  }
  @page { size: A4; margin: 15mm 14mm 16mm; }
  body { font-size: 10pt; }
  main { max-width: none; padding: 0; }
  h2 { break-after: avoid; page-break-after: avoid; }
  thead { display: table-header-group; }
  tr, .metric, dl.fields dt, dl.fields dd, ul.alerts li { break-inside: avoid; page-break-inside: avoid; }
  .banner, svg.chart { break-inside: avoid; page-break-inside: avoid; }
}
)css";

const char* tone_name(Tone tone)
{
    switch (tone) {
    case Tone::Success:
        return "success";
    case Tone::Warning:
        return "warning";
    case Tone::Error:
        return "error";
    case Tone::Neutral:
        break;
    }
    return "neutral";
}

// A mark beside the words of a tone: the words say it, the mark and the
// colour only show it at a glance.
const char* tone_mark(Tone tone)
{
    switch (tone) {
    case Tone::Success:
        return "✓";  // ✓
    case Tone::Warning:
        return "!";
    case Tone::Error:
        return "✕";  // ✕
    case Tone::Neutral:
        break;
    }
    return "–";  // –
}

std::string number(double value)
{
    return std::format("{:.1f}", value);
}

class Html {
public:
    void raw(std::string_view text) { out_ += text; }
    void words(std::string_view text) { out_ += html_escaped(text); }
    // <tag class="..."> words </tag>, the class left out where empty.
    void element(std::string_view tag, std::string_view class_name, std::string_view text)
    {
        open(tag, class_name);
        words(text);
        close(tag);
    }
    void open(std::string_view tag, std::string_view class_name = {})
    {
        out_ += '<';
        out_ += tag;
        if (!class_name.empty()) {
            out_ += " class=\"";
            out_ += class_name;
            out_ += '"';
        }
        out_ += '>';
    }
    // A line of its own after a block, none after words in a line (span).
    void close(std::string_view tag)
    {
        out_ += "</";
        out_ += tag;
        out_ += tag == "span" ? ">" : ">\n";
    }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

void fields_list(Html& html, const std::vector<Field>& fields)
{
    if (fields.empty()) {
        return;
    }
    html.raw("<dl class=\"fields\">\n");
    for (const auto& field : fields) {
        html.raw("<dt>");
        html.words(field.name);
        html.raw("</dt>");
        html.element("dd", field.code || field.path ? "code" : "", field.value);
    }
    html.raw("</dl>\n");
}

std::string pill_class(Tone tone)
{
    return std::string("pill tone-") + tone_name(tone);
}

void items_section(Html& html, const Text& text, const ResultPage& page)
{
    html.raw("<section class=\"items\">\n");
    html.element("h2", "", page.items_heading);
    if (page.items.empty()) {
        html.element("p", "dim", page.items_none);
    } else {
        html.raw("<table>\n<thead><tr><th scope=\"col\">");
        html.words(text("Status"));
        html.raw("</th><th scope=\"col\">");
        html.words(text("Path"));
        html.raw("</th><th scope=\"col\">");
        html.words(text("Detail"));
        html.raw("</th></tr></thead>\n<tbody>\n");
        for (const auto& item : page.items) {
            html.raw("<tr><td>");
            html.raw("<span class=\"" + pill_class(item.status.tone) + "\">");
            html.words(item.status.words);
            html.raw("</span></td><td class=\"code\">");
            html.words(item.path);
            html.raw("</td><td>");
            html.words(item.detail);
            html.raw("</td></tr>\n");
        }
        html.raw("</tbody>\n</table>\n");
    }
    html.raw("</section>\n");
}

// The chart of the result page (SpeedChart) as the page draws it: a line
// through each hundredth's middle, flat to the ends of the first and the last
// of a stretch read, the area below it filled; the one compared with dashed;
// the stretches read markedly slower behind them; the vertical axis's lines
// with their values, and the position in the content below.
void chart_svg(Html& html, const ChartWords& words, const SpeedChart& chart)
{
    html.raw("<p class=\"legend\"><span><i class=\"swatch now\"></i>");
    html.words(words.now);
    html.raw("</span>");
    if (chart.compared) {
        html.raw("<span><i class=\"swatch before\"></i>");
        html.words(words.before);
        html.raw("</span>");
    }
    if (!chart.slower.empty()) {
        html.raw("<span><i class=\"swatch slower\"></i>");
        html.words(words.slower);
        html.raw("</span>");
    }
    html.raw("<span class=\"unit\">");
    html.words(words.unit);
    html.raw("</span></p>\n");

    constexpr double width = 960.0;
    constexpr double height = 240.0;
    constexpr double left = 56.0;
    constexpr double right = 14.0;
    constexpr double top = 12.0;
    constexpr double bottom = 30.0;
    constexpr double plot_width = width - left - right;
    constexpr double plot_height = height - top - bottom;
    const auto x = [&](double hundredth) { return left + hundredth / 100.0 * plot_width; };
    const auto y = [&](double value) {
        return chart.top > 0.0 ? top + plot_height - value / chart.top * plot_height : top + plot_height;
    };
    const auto point = [&](double hundredth, double value) { return number(x(hundredth)) + "," + number(y(value)); };

    const auto& label = words.name;
    html.raw(std::format("<svg class=\"chart\" viewBox=\"0 0 {} {}\" role=\"img\" aria-label=\"", width, height));
    html.words(label);
    html.raw("\" xmlns=\"http://www.w3.org/2000/svg\"><title>");
    html.words(label);
    html.raw("</title>\n");
    // The lines of the axis and their values.
    for (int line = 0; chart.step > 0.0 && line * chart.step <= chart.top + chart.step / 1000.0 && line <= 20; ++line) {
        const double value = line * chart.step;
        const auto at = number(y(value));
        html.raw(std::format("<line class=\"grid\" x1=\"{}\" x2=\"{}\" y1=\"{}\" y2=\"{}\"/>", number(left),
            number(left + plot_width), at, at));
        html.raw(std::format("<text class=\"tick\" x=\"{}\" y=\"{}\" text-anchor=\"end\">", number(left - 8.0),
            number(y(value) + 4.0)));
        html.words(axis_words(value, chart.step));
        html.raw("</text>\n");
    }
    for (const int percent : {0, 25, 50, 75, 100}) {
        const char* anchor = percent == 0 ? "start" : percent == 100 ? "end" : "middle";
        html.raw(std::format("<text class=\"tick\" x=\"{}\" y=\"{}\" text-anchor=\"{}\">{}%</text>\n",
            number(x(percent)), number(height - 8.0), anchor, percent));
    }
    // The stretches read markedly slower, behind the lines.
    for (const auto& stretch : chart.slower) {
        html.raw(std::format("<rect class=\"slower\" x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"/>\n",
            number(x(stretch.from)), number(top), number(x(stretch.to + 1.0) - x(stretch.from)), number(plot_height)));
    }
    const auto trace = [&](const std::array<std::optional<double>, 100>& values, int from, int to) {
        std::string path = "M" + point(from, *values[from]);
        for (int at = from; at <= to; ++at) {
            path += " L" + point(at + 0.5, *values[at]);
        }
        path += " L" + point(to + 1.0, *values[to]);
        return path;
    };
    for (const auto& [from, to] : chart_stretches(chart.now)) {
        const auto line = trace(chart.now, from, to);
        html.raw("<path class=\"now-area\" d=\"" + line + " L" + point(to + 1.0, 0.0) + " L" + point(from, 0.0) +
                 " Z\"/>\n");
        html.raw("<path class=\"now-line\" d=\"" + line + "\"/>\n");
    }
    if (chart.compared) {
        for (const auto& [from, to] : chart_stretches(chart.before)) {
            html.raw("<path class=\"before\" d=\"" + trace(chart.before, from, to) + "\"/>\n");
        }
    }
    html.raw("</svg>\n");
}

void speed_section(Html& html, const ResultPage& page)
{
    html.raw("<section class=\"speed\">\n");
    html.element("h2", "", page.chart_words.name);
    if (page.chart) {
        chart_svg(html, page.chart_words, *page.chart);
    }
    for (const auto& line : page.speed_lines) {
        html.element("p", page.chart ? "dim" : "", line);
    }
    html.raw("</section>\n");
}

// The archive's runs, newest first, the reported one marked in words.
void runs_section(Html& html, const Text& text, const RecordedRun& recorded)
{
    html.raw("<section class=\"runs\">\n");
    html.element("h2", "", text.format("Runs of this archive ({})", grouped(recorded.history.size())));
    // When each ended, with its run under it, on lines of their own: a run's
    // id is never broken, and the time beside it leaves room for the result
    // and the note, also on paper.
    html.raw("<table>\n<thead><tr><th scope=\"col\" class=\"when\">");
    html.words(text("Finished"));
    html.element("span", "sub", text("Run"));
    html.raw("</th>");
    for (const auto* heading : {"Result", "Revision", "Note"}) {
        html.raw("<th scope=\"col\">");
        html.words(text(heading));
        html.raw("</th>");
    }
    html.raw("</tr></thead>\n<tbody>\n");
    for (auto run = recorded.history.rbegin(); run != recorded.history.rend(); ++run) {
        const bool reported = run->run_id == recorded.run.run_id;
        html.raw(reported ? "<tr class=\"this-run\"><td class=\"when\">" : "<tr><td class=\"when\">");
        html.words(local_time(run->finished, text));
        html.element("span", "sub code", run->run_id);
        html.raw("</td><td>");
        html.raw("<span class=\"" + pill_class(result_tone(run->result)) + "\">");
        html.words(result_with_mode(text, *run));
        html.raw("</span>");
        if (reported) {
            html.element("span", "this", text("This run"));
        }
        html.raw("</td><td class=\"number\">");
        html.words(std::to_string(run->revision));
        html.raw("</td><td>");
        if (const auto note = history_note(text, *run); !note.empty()) {
            html.element("span", "note", note);
        }
        for (const auto& mark : alert_marks(text, run_alerts(run->counts_json))) {
            html.element("span", "note", mark);
        }
        html.raw("</td></tr>\n");
    }
    html.raw("</tbody>\n</table>\n</section>\n");
}

void chain_section(Html& html, const Text& text, const ReportFacts& facts)
{
    html.raw("<section class=\"chain\">\n");
    html.element("h2", "", text("History chain when this report was made"));
    const auto chain = chain_words(text, facts.chain, facts.runs, facts.labels);
    html.raw(std::string("<p class=\"tone-") + tone_name(chain.tone) + "\"><span class=\"mark\" aria-hidden=\"true\">" +
             tone_mark(chain.tone) + "</span> ");
    html.words(chain.result);
    html.raw("</p>\n");
    if (!chain.limits.empty()) {
        html.element("p", "dim", chain.limits);
    }
    if (!facts.recorded.run.row_hash.empty()) {
        fields_list(html, {{text("This run's hash in the history chain (row_hash)"), facts.recorded.run.row_hash,
            false, true}});
    }
    html.raw("</section>\n");
}

void footer(Html& html, const Text& text, const ReportFacts& facts, const ShownIdentity& shown)
{
    const auto& made = facts.made;
    const auto computer = made.computer.empty() ? std::string{} : shown.shown_host(made.computer);
    html.raw("<footer>\n");
    // Two calls: the compiler checks each literal template against its values.
    html.element("p", "",
        computer.empty() ? text.format("Made {} by VeriArchive {}.", local_time(made.at, text), made.version)
                         : text.format("Made {} on the computer {} by VeriArchive {}.", local_time(made.at, text),
                               computer, made.version));
    html.element("p", "",
        text("The repository is the record: this file says what the repository recorded when the file was made. "
             "The file itself is not signed, so it cannot show a change made to it afterwards; to check it, make a "
             "new report of this run from the repository (Save report... on its result page, or veriarchive-cli "
             "report) and compare the two: this run's hash in the history chain and the chain's state."));
    html.raw("</footer>\n");
}

std::string_view file_name_safe(char c)
{
    switch (c) {
    case '\\':
    case '/':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
        return "_";
    default:
        break;
    }
    return static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? "_" : std::string_view{};
}

} // namespace

std::string html_escaped(std::string_view words)
{
    const auto valid = utf8_lossy(words);
    std::string escaped;
    escaped.reserve(valid.size());
    for (std::size_t at = 0; at < valid.size(); ++at) {
        const char c = valid[at];
        // U+0080..U+009F, the C1 controls: C2 80..C2 9F in valid UTF-8.
        if (static_cast<unsigned char>(c) == 0xC2 && at + 1 < valid.size() &&
            static_cast<unsigned char>(valid[at + 1]) <= 0x9F) {
            escaped += "\xEF\xBF\xBD";
            ++at;
            continue;
        }
        switch (c) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            if ((static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n') || c == 0x7f) {
                escaped += "�";
            } else {
                escaped += c;
            }
        }
    }
    return escaped;
}

std::optional<std::string> report_refusal(
    const Text& text, const std::filesystem::path& folder, const std::vector<AttachedArchiveRoot>& roots)
{
    for (const auto& attached : roots) {
        if (!attached.root.empty() && path_within(attached.root, folder)) {
            return report_refusal_in_root(text, folder, attached);
        }
    }
    return std::nullopt;
}

std::string report_refusal_in_root(
    const Text& text, const std::filesystem::path& folder, const AttachedArchiveRoot& holding)
{
    return text.format("{} lies in the root of {} ({}): a report saved there would add a file that its next FULL "
                       "verification reports as added. Choose a folder outside every archive.",
        path_to_utf8(folder), holding.label, path_to_utf8(holding.root));
}

std::expected<std::optional<ReportFacts>, IoError> read_report_facts(
    ArchiveStore& store, const std::string& repository_uuid, const std::string& run_id)
{
    std::optional<ReportFacts> facts;
    // In one read of the database: the run, its archive's runs, the chain
    // checked and the run it breaks at, as they are at one moment.
    const auto read = store.read_transaction([&]() -> std::expected<void, IoError> {
        auto recorded = read_recorded_run(store, run_id);
        if (!recorded) {
            return std::unexpected(recorded.error());
        }
        if (!*recorded) {
            return {};
        }
        ReportFacts found;
        found.recorded = std::move(**recorded);
        auto archives = store.archives();
        if (!archives) {
            return std::unexpected(archives.error());
        }
        // As the views name them (display_label): with the start of the
        // uuid where a label is shared (R-88).
        std::map<std::string, int> uses;
        for (const auto& archive : *archives) {
            ++uses[archive.label];
        }
        for (const auto& archive : *archives) {
            ArchiveStatus status;
            status.archive = archive;
            status.label_shared = uses[archive.label] > 1;
            found.labels[archive.archive_uuid] = display_label(status);
        }
        if (const auto label = found.labels.find(found.recorded.run.archive_uuid);
            label != found.labels.end() && !label->second.empty()) {
            found.recorded.label = label->second;
        }
        found.chain = store.check_history(repository_uuid);
        // The first run of a broken chain, by which the report names it
        // (chain_words); no other run of another archive is needed.
        if (found.chain && found.chain->first_broken_run) {
            auto broken = store.verify_run(*found.chain->first_broken_run);
            if (!broken) {
                return std::unexpected(broken.error());
            }
            if (*broken) {
                found.runs.push_back(std::move(**broken));
            }
        }
        facts = std::move(found);
        return {};
    });
    if (!read) {
        return std::unexpected(read.error());
    }
    return facts;
}

std::expected<std::string, std::string> run_report_html(
    const Text& text, const ReportFacts& facts, const ShownIdentity& shown)
{
    const auto& recorded = facts.recorded;
    if (recorded.run.result == "INCOMPLETE") {
        return std::unexpected(std::string(no_report_of_incomplete));
    }
    const auto page = result_page(text, recorded, shown);
    Html html;
    html.raw("<!doctype html>\n<html lang=\"");
    html.words(language_code(text.language()));
    html.raw("\">\n<head>\n<meta charset=\"utf-8\">\n"
             "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
             "<meta name=\"color-scheme\" content=\"light dark\">\n<meta name=\"generator\" content=\"VeriArchive ");
    html.words(facts.made.version);
    html.raw("\">\n<title>");
    html.words(text.format("Verification report · {}", page.title));
    html.raw("</title>\n<style>");
    html.raw(style);
    html.raw("</style>\n</head>\n<body>\n<main>\n");

    // The banner, as the result page's: the result with its mode, in words.
    html.raw(std::string("<header class=\"banner tone-") + tone_name(page.tone) + "\">\n");
    html.element("p", "kind", text.format("VeriArchive · {}", text("Verification report")));
    html.raw(std::string("<h1><span class=\"mark\" aria-hidden=\"true\">") + tone_mark(page.tone) + "</span> ");
    html.words(page.heading);
    html.raw("</h1>\n");
    html.element("p", "archive", page.archive);
    for (const auto& line : page.lines) {
        html.element("p", "", line);
    }
    html.raw("</header>\n");
    fields_list(html, {{text("Archive"), recorded.label}, {text("Archive UUID"), recorded.run.archive_uuid, false, true}});

    if (!page.alerts.empty()) {
        html.raw("<ul class=\"alerts\">\n");
        for (const auto& line : page.alerts) {
            html.element("li", "", line);
        }
        html.raw("</ul>\n");
    }
    if (page.items_first && page.items_shown) {
        items_section(html, text, page);
    }
    html.raw("<div class=\"metrics\">\n");
    for (const auto& metric : page.metrics) {
        html.raw("<div class=\"metric\">");
        html.element("div", "value", metric.value);
        html.element("div", "name", metric.name);
        html.raw("</div>\n");
    }
    html.raw("</div>\n");
    if (page.speed_shown) {
        speed_section(html, page);
    }
    if (!page.items_first && page.items_shown) {
        items_section(html, text, page);
    }
    html.raw("<section class=\"identity\">\n");
    html.element("h2", "", page.identity_heading);
    fields_list(html, page.identity);
    if (!page.confirmed_device.empty()) {
        html.element("h3", "", text("The device's own identity, read behind its enclosure"));
        fields_list(html, page.confirmed_device);
    }
    html.raw("</section>\n<section class=\"technical\">\n");
    html.element("h2", "", page.technical_heading);
    fields_list(html, page.technical);
    html.raw("</section>\n");
    runs_section(html, text, recorded);
    chain_section(html, text, facts);
    footer(html, text, facts, shown);
    html.raw("</main>\n</body>\n</html>\n");
    return html.take();
}

std::string report_file_name(const Text& text, const RecordedRun& recorded)
{
    const auto& run = recorded.run;
    std::string date;
    if (const auto finished = parse_timestamp(run.finished)) {
        const auto day = local_date(*finished);
        date = std::format("{:04}-{:02}-{:02}", static_cast<int>(day.year()), static_cast<unsigned>(day.month()),
            static_cast<unsigned>(day.day()));
    }
    const auto words = text.format("{} verification report {} {}", recorded.label, date, text(run.result));
    std::string name;
    for (const char c : utf8_lossy(words)) {
        const auto instead = file_name_safe(c);
        name += instead.empty() ? std::string(1, c) : std::string(instead);
    }
    // Windows takes no file name that ends with a space or a dot.
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    return name + ".html";
}

} // namespace va::app
