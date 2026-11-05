// The result page of one recorded run (decision 0046), opened over Archives
// or History as a page of their navigation view, whose back button returns
// to the page that opened it and names it. Made from the repository's
// records of the run (run_result.h): the banner with its result and mode, the
// archive and when it ended; its device alerts; its items first where it
// FAILED or PASSED_WITH_WARNINGS; what it read, how long it took and how fast;
// the read speed by position in the content, drawn with the theme's colours,
// against the verification it was compared with; then its device and
// identity, and its technical details. An INCOMPLETE run that Resume
// continues offers Resume, as the details of its archive do.
#include "gui/gui_app.h"

#include "gui/off_thread.h"
#include "gui/run_result.h"

#include "app/run_report_html.h"
#include "common/time.h"
#include "common/utf8.h"
#include "core/engine/device_alerts.h"
#include "core/engine/read_speed.h"
#include "core/write_file.h"
#include "platform/file_system.h"
#include "platform/file_writer.h"
#include "platform/media_enumerator.h"

#include <nlohmann/json.hpp>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace va::gui {
namespace {

// The items a result page lists at first, and each time more are asked for:
// a run may record every entry of an archive as differing.
constexpr std::size_t items_at_a_time = 200;
constexpr int chart_height = 220;

GtkWidget* navigation_of(const GuiState& state, ResultsOver over)
{
    return over == ResultsOver::Archives ? state.archives_page.navigation : state.history_page.navigation;
}

GtkWidget* root_of(const GuiState& state, ResultsOver over)
{
    return over == ResultsOver::Archives ? state.archives_page.root : state.history_page.root;
}

const char* page_name(ResultsOver over)
{
    return over == ResultsOver::Archives ? "archives" : "history";
}

// The pages of a navigation view, first to last, each with a reference of
// its own, which `each` is given.
template <typename Each>
void for_each_page(GtkWidget* navigation, Each each)
{
    if (!navigation) {
        return;
    }
    GListModel* stack = adw_navigation_view_get_navigation_stack(ADW_NAVIGATION_VIEW(navigation));
    const auto count = g_list_model_get_n_items(stack);
    for (guint index = 0; index < count; ++index) {
        auto* page = static_cast<AdwNavigationPage*>(g_list_model_get_item(stack, index));
        each(page);
        g_object_unref(page);
    }
    g_object_unref(stack);
}

// An archive as Archives lists it: its label (display_label) and the volume
// label of its current carrier as the views show it.
struct ListedName {
    std::string label;
    std::string volume_label;
};

std::optional<ListedName> listed_name(const GuiState& state, const Uuid& archive)
{
    for (const auto& status : state.archives_page.statuses) {
        if (status.archive.archive_uuid == archive) {
            return ListedName{display_label(status),
                status.header ? displayed_identity(status.header->identity, state.identity_shown).volume_label
                              : std::string{}};
        }
    }
    return std::nullopt;
}

// Names the archive of `recorded` as the list names it, where the list has
// read it; otherwise by its record, as app::read_recorded_run named it.
void name_as(const std::optional<ListedName>& listed, RecordedRun& recorded)
{
    if (!listed) {
        return;
    }
    if (!listed->label.empty()) {
        recorded.label = listed->label;
    }
    recorded.volume_label = listed->volume_label;
}

void name_as_listed(const GuiState& state, RecordedRun& recorded)
{
    name_as(listed_name(state, recorded.run.archive_uuid), recorded);
}

// What the repository records of the run `run_id`, or why it cannot be read.
std::expected<RecordedRun, Report> read_recorded(GuiState& state, const std::string& run_id)
{
    const auto cannot = [](std::string message) -> std::unexpected<Report> {
        return std::unexpected<Report>([message = std::move(message)](const app::Text& text) {
            return text.format("Cannot read this run: {}", text(message));
        });
    };
    if (!state.session) {
        return std::unexpected(fixed_message("Open a repository first (Settings)."));
    }
    auto store = ArchiveStore::open_read_only(state.session->context.repo);
    if (!store) {
        return cannot(store.error().message);
    }
    auto read = app::read_recorded_run(*store, run_id);
    if (!read) {
        return cannot(read.error().message);
    }
    if (!*read) {
        return std::unexpected(fixed_message("This run is no longer recorded."));
    }
    // The archive as the list names it; by its record where the list has
    // not read it.
    auto recorded = std::move(**read);
    name_as_listed(state, recorded);
    return recorded;
}

// ---- The read speed chart ---------------------------------------------------

// What the chart draws, and the swatches of its legend that it takes its
// colours from, so that they follow the light and the dark style alike.
struct ChartShown {
    SpeedChart chart;
    GtkWidget* now = nullptr;
    GtkWidget* before = nullptr;
    GtkWidget* slower = nullptr;
};

PangoLayout* small_layout(GtkWidget* widget, const std::string& words)
{
    PangoLayout* layout = gtk_widget_create_pango_layout(widget, words.c_str());
    PangoAttrList* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_scale_new(PANGO_SCALE_SMALL));
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
    return layout;
}

void set_colour(cairo_t* cr, const GdkRGBA& colour, double alpha = 1.0)
{
    cairo_set_source_rgba(cr, colour.red, colour.green, colour.blue, colour.alpha * alpha);
}

GdkRGBA colour_of(GtkWidget* widget, const GdkRGBA& otherwise)
{
    if (!widget) {
        return otherwise;
    }
    GdkRGBA colour;
    gtk_widget_get_color(widget, &colour);
    return colour;
}

void draw_chart(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data)
{
    const auto& shown = *static_cast<const ChartShown*>(data);
    const auto& chart = shown.chart;
    GtkWidget* widget = GTK_WIDGET(area);
    GdkRGBA ink;
    gtk_widget_get_color(widget, &ink);
    const auto now_ink = colour_of(shown.now, ink);
    const auto before_ink = colour_of(shown.before, ink);
    const auto slower_ink = colour_of(shown.slower, ink);

    // The values of the vertical axis, written right-aligned left of the plot.
    std::vector<double> ticks;
    for (int line = 0; chart.step > 0.0 && line * chart.step <= chart.top + chart.step / 1000.0 && line <= 20; ++line) {
        ticks.push_back(line * chart.step);
    }
    int widest = 0;
    int line_height = 0;
    for (const auto value : ticks) {
        PangoLayout* layout = small_layout(widget, axis_words(value, chart.step));
        int label_width = 0;
        int label_height = 0;
        pango_layout_get_pixel_size(layout, &label_width, &label_height);
        widest = std::max(widest, label_width);
        line_height = std::max(line_height, label_height);
        g_object_unref(layout);
    }
    const double left = widest + 10.0;
    const double right = 10.0;
    const double top = line_height / 2.0 + 2.0;
    const double bottom = line_height + 8.0;
    const double plot_width = width - left - right;
    const double plot_height = height - top - bottom;
    if (plot_width < 20.0 || plot_height < 20.0 || chart.top <= 0.0) {
        return;
    }
    const auto x = [&](double hundredth) { return left + hundredth / 100.0 * plot_width; };
    const auto y = [&](double value) { return top + plot_height - value / chart.top * plot_height; };

    // The lines of the axis and their values.
    cairo_set_line_width(cr, 1.0);
    for (const auto value : ticks) {
        const double at = std::round(y(value)) + 0.5;
        set_colour(cr, ink, 0.18);
        cairo_move_to(cr, left, at);
        cairo_line_to(cr, left + plot_width, at);
        cairo_stroke(cr);
        PangoLayout* layout = small_layout(widget, axis_words(value, chart.step));
        int label_width = 0;
        int label_height = 0;
        pango_layout_get_pixel_size(layout, &label_width, &label_height);
        set_colour(cr, ink);
        cairo_move_to(cr, left - 6.0 - label_width, at - label_height / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }
    for (const int percent : {0, 25, 50, 75, 100}) {
        PangoLayout* layout = small_layout(widget, std::to_string(percent) + "%");
        int label_width = 0;
        int label_height = 0;
        pango_layout_get_pixel_size(layout, &label_width, &label_height);
        const double at = std::clamp(x(percent) - label_width / 2.0, 0.0, width - static_cast<double>(label_width));
        set_colour(cr, ink);
        cairo_move_to(cr, at, top + plot_height + 6.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    // The stretches read markedly slower, behind the lines.
    for (const auto& stretch : chart.slower) {
        set_colour(cr, slower_ink, 0.18);
        cairo_rectangle(cr, x(stretch.from), top, x(stretch.to + 1.0) - x(stretch.from), plot_height);
        cairo_fill(cr);
    }

    // A line through each hundredth's middle, flat to the ends of the
    // first and the last of a stretch read.
    const auto trace = [&](const std::array<std::optional<double>, 100>& values, int from, int to) {
        cairo_move_to(cr, x(from), y(*values[from]));
        for (int at = from; at <= to; ++at) {
            cairo_line_to(cr, x(at + 0.5), y(*values[at]));
        }
        cairo_line_to(cr, x(to + 1.0), y(*values[to]));
    };
    for (const auto& [from, to] : chart_stretches(chart.now)) {
        trace(chart.now, from, to);
        cairo_line_to(cr, x(to + 1.0), y(0.0));
        cairo_line_to(cr, x(from), y(0.0));
        cairo_close_path(cr);
        set_colour(cr, now_ink, 0.22);
        cairo_fill(cr);
        trace(chart.now, from, to);
        set_colour(cr, now_ink);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
    }
    if (chart.compared) {
        const double dashes[] = {4.0, 3.0};
        cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_set_line_width(cr, 1.3);
        for (const auto& [from, to] : chart_stretches(chart.before)) {
            trace(chart.before, from, to);
            set_colour(cr, before_ink);
            cairo_stroke(cr);
        }
        cairo_set_dash(cr, nullptr, 0, 0.0);
    }
}

// A swatch of the legend in a chart colour (`kind`), and its words.
GtkWidget* legend_item(GtkWidget* legend, const char* kind, const std::string& words, GtkWidget** swatch)
{
    GtkWidget* item = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    *swatch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(*swatch, "va-swatch");
    gtk_widget_add_css_class(*swatch, kind);
    gtk_widget_set_valign(*swatch, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(item), *swatch);
    GtkWidget* label = gtk_label_new(words.c_str());
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_add_css_class(label, "caption");
    gtk_box_append(GTK_BOX(item), label);
    gtk_box_append(GTK_BOX(legend), item);
    return item;
}

GtkWidget* speed_chart_widget(const app::ChartWords& words, const SpeedChart& chart, const std::string& described)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    auto shown = std::make_unique<ChartShown>();
    shown->chart = chart;
    GtkWidget* legend = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    legend_item(legend, "va-chart-now", words.now, &shown->now);
    if (chart.compared) {
        legend_item(legend, "va-chart-before", words.before, &shown->before);
    }
    if (!chart.slower.empty()) {
        legend_item(legend, "va-chart-slower", words.slower, &shown->slower);
    }
    GtkWidget* unit = gtk_label_new(words.unit.c_str());
    gtk_widget_add_css_class(unit, "dim-label");
    gtk_widget_add_css_class(unit, "caption");
    gtk_widget_set_hexpand(unit, TRUE);
    gtk_label_set_xalign(GTK_LABEL(unit), 1.0f);
    gtk_box_append(GTK_BOX(legend), unit);
    gtk_box_append(GTK_BOX(box), legend);

    GtkWidget* area = gtk_drawing_area_new();
    gtk_widget_add_css_class(area, "va-chart");
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), chart_height);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(area), GTK_ACCESSIBLE_PROPERTY_LABEL, words.name.c_str(),
        GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, described.c_str(), -1);
    // What it draws, also for the smoke test's check (check_sample_chart).
    g_object_set_data(G_OBJECT(area), "va-chart", shown.get());
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_chart, shown.release(),
        +[](gpointer data) { delete static_cast<ChartShown*>(data); });
    // The swatches follow a change of the style at once; the chart draws
    // itself again with their new colours.
    g_signal_connect_object(adw_style_manager_get_default(), "notify::dark",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer drawn) { gtk_widget_queue_draw(GTK_WIDGET(drawn)); }), area,
        GConnectFlags{});
    gtk_box_append(GTK_BOX(box), area);
    return box;
}

// ---- The page ---------------------------------------------------------------

GtkWidget* group(const std::string& title)
{
    GtkWidget* shown = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(shown), title.c_str());
    return shown;
}

GtkWidget* item_row(const ResultItem& item)
{
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    GtkWidget* pill = make_pill(item.status);
    gtk_widget_set_valign(pill, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), pill);
    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(words, TRUE);
    GtkWidget* path = make_words(item.path, "monospace", true);
    gtk_box_append(GTK_BOX(words), path);
    if (!item.detail.empty()) {
        GtkWidget* detail = make_words(item.detail, "dim-label", true);
        gtk_widget_add_css_class(detail, "caption");
        gtk_box_append(GTK_BOX(words), detail);
    }
    gtk_box_append(GTK_BOX(box), words);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

// The items of a page, listed a part at a time.
struct ItemsShown {
    std::shared_ptr<const ResultPage> page;
    GtkWidget* group = nullptr;
    GtkWidget* more = nullptr;  // the row that lists more, while there are more
    std::size_t listed = 0;
};

struct MoreItems {
    GuiState* state;
    ItemsShown* shown;  // owned by the group, which holds the row
};

// Lists the next part of the items, then a row that lists more where there
// are more: a click or Enter on it lists the part after. Returns the first
// row it listed.
GtkWidget* list_more_items(GuiState& state, ItemsShown& shown)
{
    const auto& text = state.text;
    const auto& items = shown.page->items;
    const auto until = std::min(items.size(), shown.listed + items_at_a_time);
    if (shown.more) {
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(shown.group), shown.more);
        shown.more = nullptr;
    }
    GtkWidget* first = nullptr;
    for (; shown.listed < until; ++shown.listed) {
        GtkWidget* row = item_row(items[shown.listed]);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown.group), row);
        first = first ? first : row;
    }
    if (shown.listed < items.size()) {
        const auto rest = items.size() - shown.listed;
        shown.more = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(shown.more), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(shown.more),
            text.format("Show {} more items (of {})", grouped(std::min(rest, items_at_a_time)), grouped(rest))
                .c_str());
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(shown.more), TRUE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(shown.more), gtk_image_new_from_icon_name("go-down-symbolic"));
        g_signal_connect_data(shown.more, "activated",
            G_CALLBACK(+[](AdwActionRow*, gpointer data) {
                auto* more = static_cast<MoreItems*>(data);
                // Copies: the row goes as the next part is listed, and the
                // keyboard focus with it to the first item listed, as in
                // History, never out of the page.
                auto* state = more->state;
                auto* items = more->shown;
                guarded(
                    [&] {
                        if (GtkWidget* first = list_more_items(*state, *items)) {
                            gtk_widget_grab_focus(first);
                        }
                    },
                    [](const Report&) {});
            }),
            new MoreItems{&state, &shown}, +[](gpointer data, GClosure*) { delete static_cast<MoreItems*>(data); },
            GConnectFlags{});
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown.group), shown.more);
    }
    return first;
}

GtkWidget* items_group(GuiState& state, const std::shared_ptr<const ResultPage>& page)
{
    GtkWidget* shown_group = group(page->items_heading);
    if (page->items.empty()) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), page->items_none.c_str());
        gtk_widget_add_css_class(row, "dim-label");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown_group), row);
        return shown_group;
    }
    auto* shown = new ItemsShown{page, shown_group, nullptr, 0};
    g_object_set_data_full(G_OBJECT(shown_group), "va-items", shown,
        +[](gpointer data) { delete static_cast<ItemsShown*>(data); });
    list_more_items(state, *shown);
    return shown_group;
}

GtkWidget* metrics_box(const std::vector<Field>& metrics)
{
    GtkWidget* flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_widget_add_css_class(flow, "card");
    for (const auto& metric : metrics) {
        GtkWidget* cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_add_css_class(cell, "va-metric");
        // Not selectable, nor the banner's words: the first of the page to
        // take the focus would show its words selected. The technical
        // details hold what may be copied.
        GtkWidget* value = make_words(metric.value, "title-3");
        gtk_widget_add_css_class(value, "numeric");
        gtk_box_append(GTK_BOX(cell), value);
        GtkWidget* name = make_words(metric.name, "dim-label");
        gtk_widget_add_css_class(name, "caption");
        gtk_box_append(GTK_BOX(cell), name);
        gtk_flow_box_append(GTK_FLOW_BOX(flow), cell);
        // Words to read, not a choice to make.
        gtk_widget_set_focusable(gtk_widget_get_parent(cell), FALSE);
    }
    return flow;
}

GtkWidget* expander(const std::string& title, const std::vector<Field>& fields)
{
    GtkWidget* row = adw_expander_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
    adw_expander_row_add_row(ADW_EXPANDER_ROW(row), make_fields_row(fields));
    return row;
}

// The banner: the result with its mode, the archive and when the run ended,
// and what else it says; Resume for an INCOMPLETE run (shown by
// show_results_operations).
GtkWidget* banner(GuiState& state, const ResultPage& page, GtkWidget* page_widget)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(box, "va-banner");
    gtk_widget_add_css_class(box, tone_class(page.tone));
    GtkWidget* icon = gtk_image_new_from_icon_name(page.incomplete ? "media-playback-pause-symbolic" : tone_icon(page.tone));
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 22);
    gtk_widget_add_css_class(icon, "va-banner-icon");
    gtk_widget_add_css_class(icon, tone_class(page.tone));
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), icon);
    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(words, TRUE);
    gtk_widget_set_valign(words, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(words), make_words(page.heading, "title-3"));
    GtkWidget* archive = make_words(page.archive, "dim-label");
    gtk_widget_add_css_class(archive, "numeric");
    gtk_box_append(GTK_BOX(words), archive);
    for (const auto& line : page.lines) {
        GtkWidget* label = make_words(line, "dim-label");
        gtk_widget_add_css_class(label, "numeric");
        gtk_box_append(GTK_BOX(words), label);
    }
    gtk_box_append(GTK_BOX(box), words);
    if (page.incomplete) {
        GtkWidget* resume = gtk_button_new_with_label(state.text("Resume").c_str());
        gtk_widget_add_css_class(resume, "suggested-action");
        gtk_widget_set_valign(resume, GTK_ALIGN_CENTER);
        gtk_widget_set_visible(resume, FALSE);
        g_object_set_data(G_OBJECT(page_widget), "va-resume", resume);
        g_signal_connect(resume, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            GtkWidget* shown = gtk_widget_get_ancestor(GTK_WIDGET(clicked), ADW_TYPE_NAVIGATION_PAGE);
            const auto* run = shown ? static_cast<const VerifyRunRecord*>(g_object_get_data(G_OBJECT(shown), "va-run-record"))
                                    : nullptr;
            if (!run) {
                return;
            }
            guarded(
                [&] {
                    const auto stopped = stopped_run(state.text, state.operations.watches, *run,
                        std::chrono::steady_clock::now(),
                        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
                    if (!stopped) {
                        show_message(state, fixed_message("This verification is no longer listed as unfinished."));
                        return;
                    }
                    // Copies: showing Archives closes this page when it is open over it.
                    const auto archive = run->archive_uuid;
                    const auto key = stopped->key;
                    // The archive's details show the job, as their Resume does.
                    show_page(state, "archives");
                    select_archive(state, archive, true);
                    resume_in_details(state, archive, key);
                },
                [&](const Report& report) { show_message(state, report); });
        }), &state);
        gtk_box_append(GTK_BOX(box), resume);
    } else {
        // Decision 0048: the verification report of a run with a result.
        GtkWidget* save = gtk_button_new_with_label(state.text("Save report...").c_str());
        gtk_widget_set_valign(save, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(save,
            state.text("Saves what this page says as one HTML file, a verification report, which a browser opens "
                       "and which can be sent to others.")
                .c_str());
        g_object_set_data(G_OBJECT(page_widget), "va-save-report", save);
        g_signal_connect(save, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            GtkWidget* shown = gtk_widget_get_ancestor(GTK_WIDGET(clicked), ADW_TYPE_NAVIGATION_PAGE);
            const char* run = shown ? static_cast<const char*>(g_object_get_data(G_OBJECT(shown), "va-run")) : nullptr;
            const char* name =
                shown ? static_cast<const char*>(g_object_get_data(G_OBJECT(shown), "va-report-name")) : nullptr;
            if (!run || !name) {
                return;
            }
            guarded([&] { save_report(state, run, name); }, [&](const Report& report) { show_message(state, report); });
        }), &state);
        gtk_box_append(GTK_BOX(box), save);
    }
    return box;
}

// The page's content, or why the run cannot be shown.
GtkWidget* page_content(GuiState& state, const std::string& run_id, GtkWidget* page_widget, std::string& title)
{
    const auto& text = state.text;
    auto recorded = read_recorded(state, run_id);
    if (!recorded) {
        GtkWidget* status = adw_status_page_new();
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "dialog-warning-symbolic");
        set_status_description(status, recorded.error()(text));
        title = text("Verification");
        return status;
    }
    const auto page = std::make_shared<const ResultPage>(result_page(text, *recorded, state.identity_shown));
    title = page->title;
    g_object_set_data_full(G_OBJECT(page_widget), "va-run-record", new VerifyRunRecord(recorded->run),
        +[](gpointer data) { delete static_cast<VerifyRunRecord*>(data); });
    // The name Save report... proposes, in the window's language.
    g_object_set_data_full(G_OBJECT(page_widget), "va-report-name",
        g_strdup(app::report_file_name(text, *recorded).c_str()), g_free);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_box_append(GTK_BOX(box), banner(state, *page, page_widget));
    // Decision 0044: the device alerts, as the run's report gave them.
    for (const auto& line : page->alerts) {
        GtkWidget* alert = make_line_box("va-error", "dialog-warning-symbolic", "error");
        GtkWidget* words = make_words(line, nullptr, true);
        gtk_widget_set_hexpand(words, TRUE);
        gtk_box_append(GTK_BOX(alert), words);
        gtk_box_append(GTK_BOX(box), alert);
    }
    if (page->items_first && page->items_shown) {
        gtk_box_append(GTK_BOX(box), items_group(state, page));
    }
    gtk_box_append(GTK_BOX(box), metrics_box(page->metrics));
    if (page->speed_shown) {
        GtkWidget* speed = group(page->chart_words.name);
        GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_add_css_class(card, "card");
        GtkWidget* inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_margin_start(inner, 14);
        gtk_widget_set_margin_end(inner, 14);
        gtk_widget_set_margin_top(inner, 12);
        gtk_widget_set_margin_bottom(inner, 12);
        std::string described;
        for (const auto& line : page->speed_lines) {
            described += (described.empty() ? "" : " ") + line;
        }
        if (page->chart) {
            gtk_box_append(GTK_BOX(inner), speed_chart_widget(page->chart_words, *page->chart, described));
        }
        for (const auto& line : page->speed_lines) {
            gtk_box_append(GTK_BOX(inner), make_words(line, page->chart ? "dim-label" : nullptr));
        }
        gtk_box_append(GTK_BOX(card), inner);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(speed), card);
        gtk_box_append(GTK_BOX(box), speed);
    }
    if (!page->items_first && page->items_shown) {
        gtk_box_append(GTK_BOX(box), items_group(state, page));
    }
    GtkWidget* more = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(more), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(more, "boxed-list");
    gtk_list_box_append(GTK_LIST_BOX(more), expander(page->identity_heading, page->identity));
    gtk_list_box_append(GTK_LIST_BOX(more), expander(page->technical_heading, page->technical));
    gtk_box_append(GTK_BOX(box), more);

    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 700);
    adw_clamp_set_child(ADW_CLAMP(clamp), box);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
    gtk_widget_set_vexpand(scroller, TRUE);
    return scroller;
}

// The result page of `run_id`, opened over `over`: above it, under the
// window's own header bar, the way back to that page, which names it, as the
// details' way back to the list of Archives does. No second header bar: in a
// narrow window its arrow alone would sit under the header's arrow back to
// the sidebar, which goes elsewhere. The navigation view's own Escape,
// Alt+Left and back button of a mouse go back as well.
AdwNavigationPage* make_result_page(GuiState& state, ResultsOver over, const std::string& run_id)
{
    GtkWidget* view = adw_toolbar_view_new();
    AdwNavigationPage* page = adw_navigation_page_new(view, "");
    g_object_set_data_full(G_OBJECT(page), "va-run", g_strdup(run_id.c_str()), g_free);
    GtkWidget* root = root_of(state, over);
    const char* previous = root ? adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(root)) : nullptr;
    GtkWidget* back = gtk_button_new();
    GtkWidget* face = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(face), "go-previous-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(face),
        previous && *previous ? previous : state.text(over == ResultsOver::Archives ? "Archives" : "History").c_str());
    gtk_button_set_child(GTK_BUTTON(back), face);
    gtk_widget_add_css_class(back, "flat");
    gtk_widget_set_halign(back, GTK_ALIGN_START);
    gtk_widget_set_margin_start(back, 18);
    gtk_widget_set_margin_top(back, 6);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(back), "navigation.pop");
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), back);
    adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(view), ADW_TOOLBAR_FLAT);
    std::string title;
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page_content(state, run_id, GTK_WIDGET(page), title));
    adw_navigation_page_set_title(page, title.c_str());
    return page;
}

bool visible_page_is(GtkWidget* navigation, const std::string& run_id)
{
    AdwNavigationPage* visible = adw_navigation_view_get_visible_page(ADW_NAVIGATION_VIEW(navigation));
    const char* run = visible ? static_cast<const char*>(g_object_get_data(G_OBJECT(visible), "va-run")) : nullptr;
    return run && run_id == run;
}

// ---- The verification report (decision 0048) --------------------------------

// Writes the verification report of `facts` into `file`, with the platform's
// durable writer, which replaces a file there as a whole (the save dialog
// asked first); nothing where it was written, else why not.
Report write_report(
    const app::Text& text, const app::ReportFacts& facts, const app::ShownIdentity& shown, const std::filesystem::path& file)
{
    const auto html = app::run_report_html(text, facts, shown);
    if (!html) {
        return fixed_message(html.error());
    }
    const auto writer = platform::make_file_writer();
    if (auto written = write_file_atomically(*writer, file, *html); !written) {
        return fixed_message(written.error().message);
    }
    return {};
}

// What a report is made of, taken on the main thread: where the repository
// is, the run, the archives as Archives lists them, the language, the
// identity the views show, who makes it, and the file to write.
struct ReportJob {
    RepositoryLayout layout;
    std::string repository_uuid;
    std::string run_id;
    std::map<Uuid, ListedName> listed;
    app::Text text;
    IdentityShown shown = IdentityShown::Recorded;
    app::ReportMade made;
    std::filesystem::path file;
};

// Why the report was not written (on the thread of read_off_thread): nothing
// where it was; `refused` where its folder lies in an archive's root.
struct ReportWritten {
    Report failure;
    bool refused = false;
};

ReportWritten make_report(const ReportJob& job)
{
    ReportWritten written;
    const auto cannot = [](std::string message) {
        return Report([message = std::move(message)](const app::Text& text) {
            return text.format("Cannot read this run: {}", text(message));
        });
    };
    // Only read: a database that went missing is reported, not created anew.
    auto store = ArchiveStore::open_read_only(job.layout);
    if (!store) {
        written.failure = cannot(store.error().message);
        return written;
    }
    // Never into an archive's root, as the folder's volume is now: the
    // latest read of the attached devices may not know it yet (a volume
    // attached since), as the export checks it again.
    const auto folder = job.file.parent_path();
    const auto media = platform::make_media_enumerator();
    const auto repository_files = platform::make_file_system_access();
    if (const auto holding = app::roots_holding(*store, *repository_files, *media, job.layout, folder);
        !holding.empty()) {
        written.failure = [folder, root = holding.front()](const app::Text& text) {
            return app::report_refusal_in_root(text, folder, root);
        };
        written.refused = true;
        return written;
    }
    auto facts = app::read_report_facts(*store, job.repository_uuid, job.run_id);
    if (!facts) {
        written.failure = cannot(facts.error().message);
        return written;
    }
    if (!*facts) {
        written.failure = fixed_message("This run is no longer recorded.");
        return written;
    }
    // The archive as the page names it.
    if (const auto listed = job.listed.find((*facts)->recorded.run.archive_uuid); listed != job.listed.end()) {
        name_as(listed->second, (*facts)->recorded);
    }
    (*facts)->made = job.made;
    (*facts)->made.at = current_timestamp();
    written.failure = write_report(job.text, **facts, shown_identity(job.shown), job.file);
    return written;
}

// Opens `file` as the system opens such a file (a browser for a report).
void open_report(GuiState& state, const std::filesystem::path& file)
{
    struct Opening {
        GuiState* state;
        std::string name;
    };
    GFile* opened = g_file_new_for_path(path_to_utf8(file).c_str());
    GtkFileLauncher* launcher = gtk_file_launcher_new(opened);
    g_object_unref(opened);
    gtk_file_launcher_launch(launcher, state.window, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Opening> opening(static_cast<Opening*>(data));
            GError* error = nullptr;
            if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source), result, &error)) {
                auto& state = *opening->state;
                if (state.window && error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
                    show_toast(state, state.text.format("Cannot open {}: {}", opening->name, error->message));
                }
            }
            g_clear_error(&error);
            g_object_unref(source);
        },
        new Opening{&state, path_to_utf8(file.filename())});
}

// The folder the save dialog opens in: the last one a report was saved into
// in this window, else the Documents folder, else the home folder.
std::filesystem::path report_folder(const GuiState& state)
{
    if (state.report_folder) {
        return *state.report_folder;
    }
    if (const char* documents = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS); documents && *documents) {
        return path_from_utf8(documents);
    }
    const char* home = g_get_home_dir();
    return home ? path_from_utf8(home) : std::filesystem::path{};
}

} // namespace

void save_report(GuiState& state, const std::string& run_id, const std::string& proposed)
{
    struct Asked {
        GuiState* state;
        std::string run_id;
    };
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, state.text("Save the verification report").c_str());
    gtk_file_dialog_set_initial_name(dialog, proposed.c_str());
    if (const auto folder = report_folder(state); !folder.empty()) {
        // GLib's file names are UTF-8 on Windows; on Linux the path's bytes.
        GFile* initial = g_file_new_for_path(path_to_utf8(folder).c_str());
        gtk_file_dialog_set_initial_folder(dialog, initial);
        g_object_unref(initial);
    }
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, state.text("HTML file").c_str());
    gtk_file_filter_add_suffix(filter, "html");
    gtk_file_filter_add_suffix(filter, "htm");
    GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filters);
    g_object_unref(filter);
    // The dialog asks before it replaces a file there.
    gtk_file_dialog_save(dialog, state.window, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Asked> asked(static_cast<Asked*>(data));
            GFile* chosen = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
            g_object_unref(source);
            if (!chosen) {
                return;  // cancelled
            }
            gchar* path = g_file_get_path(chosen);
            g_object_unref(chosen);
            auto& state = *asked->state;
            if (!path || !state.window) {
                g_free(path);
                return;
            }
            const auto file = path_from_utf8(path);
            g_free(path);
            guarded([&] { save_report_to(state, asked->run_id, file); },
                [&](const Report& report) { show_message(state, report); });
        },
        new Asked{&state, run_id});
}

void save_report_to(GuiState& state, const std::string& run_id, const std::filesystem::path& file)
{
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    // Never into an archive's root, as the latest read of the attached
    // devices found them (make_report asks again, of the folder's volume).
    const auto folder = file.parent_path();
    if (auto roots = attached_roots(state); app::report_refusal(state.text, folder, roots)) {
        const Report refused = [folder, roots = std::move(roots)](const app::Text& text) {
            return app::report_refusal(text, folder, roots).value_or(std::string{});
        };
        state.report_saved = GuiState::ReportSaved{file, refused};
        show_message(state, refused);
        return;
    }
    ReportJob job;
    job.layout = state.session->context.repo;
    job.repository_uuid = state.session->context.repository_uuid;
    job.run_id = run_id;
    for (const auto& status : state.archives_page.statuses) {
        if (auto listed = listed_name(state, status.archive.archive_uuid)) {
            job.listed.emplace(status.archive.archive_uuid, std::move(*listed));
        }
    }
    job.text = state.text;
    job.shown = state.identity_shown;
    job.made = {{}, state.host.host, state.host.app_version};
    job.file = file;
    ++state.reports_saving;
    state.report_saved.reset();
    // The file as it was chosen: a report that could not be made at all
    // (an exception of make_report) still names it.
    read_off_thread<ReportWritten>(
        state, [job = std::move(job)] { return make_report(job); },
        [file](GuiState& state, ReportWritten& written) {
            --state.reports_saving;
            state.report_saved = GuiState::ReportSaved{file, written.failure};
            const auto name = path_to_utf8(file.filename());
            if (written.refused) {
                show_message(state, written.failure);
                return;
            }
            if (written.failure) {
                show_toast(state, state.text.format("The report {} was not saved: {}", name, written.failure(state.text)));
                return;
            }
            // Where the save dialog opens next.
            state.report_folder = file.parent_path();
            show_toast(state, state.text.format("Saved report: {}", name),
                ToastAction{state.text("Open"), [file](GuiState& state) { open_report(state, file); }});
        });
}

void open_result(GuiState& state, ResultsOver over, const std::string& run_id)
{
    GtkWidget* navigation = navigation_of(state, over);
    if (!navigation) {
        return;
    }
    // The page it opens over, itself, and in a narrow window in place of the
    // sidebar; then the result page over it.
    show_page(state, page_name(over));
    if (visible_page_is(navigation, run_id)) {
        return;
    }
    AdwNavigationPage* page = make_result_page(state, over, run_id);
    adw_navigation_view_push(ADW_NAVIGATION_VIEW(navigation), page);
    // The focus goes into the page, to its first button (back), not to the
    // page it covers or the words of an item.
    gtk_widget_grab_focus(GTK_WIDGET(page));
    show_results_operations(state);
    // An INCOMPLETE run offers Resume as the latest read of the unfinished
    // operations, which Archives makes while it is shown, finds it.
    start_poll(state);
}

void open_result_of(GuiState& state, const Uuid& archive, const std::string& run_id)
{
    // The details, in place of the list in a narrow window, so that back
    // returns to them.
    select_archive(state, archive, true);
    open_result(state, ResultsOver::Archives, run_id);
}

std::vector<std::string> results_open(const GuiState& state, ResultsOver over)
{
    std::vector<std::string> runs;
    for_each_page(navigation_of(state, over), [&](AdwNavigationPage* page) {
        if (const char* run = static_cast<const char*>(g_object_get_data(G_OBJECT(page), "va-run"))) {
            runs.emplace_back(run);
        }
    });
    return runs;
}

void open_results_again(GuiState& state, ResultsOver over, const std::vector<std::string>& runs)
{
    GtkWidget* navigation = navigation_of(state, over);
    GtkWidget* root = root_of(state, over);
    if (!navigation || !root || runs.empty()) {
        return;
    }
    std::vector<AdwNavigationPage*> pages{ADW_NAVIGATION_PAGE(root)};
    for (const auto& run : runs) {
        pages.push_back(make_result_page(state, over, run));
    }
    adw_navigation_view_replace(ADW_NAVIGATION_VIEW(navigation), pages.data(), static_cast<int>(pages.size()));
    show_results_operations(state);
}

void show_results_operations(GuiState& state)
{
    const auto now = std::chrono::steady_clock::now();
    const auto wall_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    for (const auto over : {ResultsOver::Archives, ResultsOver::History}) {
        for_each_page(navigation_of(state, over), [&](AdwNavigationPage* page) {
            auto* resume = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(page), "va-resume"));
            const auto* run = static_cast<const VerifyRunRecord*>(g_object_get_data(G_OBJECT(page), "va-run-record"));
            if (!resume || !run) {
                return;
            }
            // As the details offer Resume: not while the archive's job (the
            // last verification its details started, by its uuid or by a
            // root) or any job of the window waits or works on the archive.
            const auto started = state.verification_jobs.find(run->archive_uuid);
            const auto* active = started == state.verification_jobs.end() ? nullptr : state.jobs.find(started->second);
            const bool busy =
                (active && active->state != JobState::Ended) || state.jobs.reads(std::nullopt, run->archive_uuid);
            const bool offered =
                state.session && !busy && stopped_run(state.text, state.operations.watches, *run, now, wall_now);
            gtk_widget_set_visible(resume, offered);
        });
    }
}

void check_sample_chart(GuiState& state)
{
    // Sample data, made up for this check, of no archive or device: 100
    // hundredths of 1 GB each, read in 12 s, those from 12% to 25% in 36 s,
    // against an earlier read of 11 s each, so that the comparison finds
    // that stretch markedly slower.
    ReadSpeed now;
    now.content_bytes = 100'000'000'000;
    now.devices = {"sample SAMPLE-SERIAL"};
    ReadSpeed before = now;
    for (std::uint32_t hundredth = 0; hundredth < 100; ++hundredth) {
        now.parts.push_back({hundredth, 1'000'000'000, hundredth >= 12 && hundredth <= 24 ? 36'000u : 12'000u});
        before.parts.push_back({hundredth, 1'000'000'000, 11'000});
    }
    const auto compared = compare_read_speed(now, before);
    const auto chart = speed_chart(now, before, compared ? compared->slower_parts : std::vector<SlowerStretch>{});
    if (!chart || !chart->compared || chart->slower.empty()) {
        throw std::runtime_error("the sample read speed chart has no earlier read or no slower stretch to draw");
    }
    // In a window of its own, never shown, whose style it takes as a result
    // page does.
    GtkWidget* window = gtk_window_new();
    GtkWidget* shown = speed_chart_widget(app::chart_words(state.text), *chart, "SAMPLE");
    gtk_window_set_child(GTK_WINDOW(window), shown);
    GtkWidget* area = gtk_widget_get_last_child(shown);
    auto* drawn = area ? static_cast<ChartShown*>(g_object_get_data(G_OBJECT(area), "va-chart")) : nullptr;
    const char* folder = std::getenv("VERIARCHIVE_GUI_SMOKE_CHART");
    AdwStyleManager* style = adw_style_manager_get_default();
    const auto scheme = adw_style_manager_get_color_scheme(style);
    std::string failure;
    std::vector<GdkRGBA> inks;  // the colours of the axis and of this verification's line, in each style
    constexpr int width = 720;
    // Each on about the window's background of its style, as a picture to
    // look at (cairo ARGB32 pixels: alpha, red, green, blue).
    for (const auto& [each, name, ground] : {std::tuple{ADW_COLOR_SCHEME_FORCE_LIGHT, "light", 0xfffafafau},
             std::tuple{ADW_COLOR_SCHEME_FORCE_DARK, "dark", 0xff242424u}}) {
        if (!drawn) {
            failure = "the read speed chart holds nothing to draw";
            break;
        }
        adw_style_manager_set_color_scheme(style, each);
        // A window that is shown takes the new style as it next draws; this
        // one, never shown, takes it once its widgets are told to, each after
        // the widgets it lies in, whose colours it inherits.
        for (GtkWidget* widget : {area, drawn->now, drawn->before, drawn->slower}) {
            std::vector<GtkWidget*> chain;
            for (GtkWidget* at = widget; at; at = gtk_widget_get_parent(at)) {
                chain.push_back(at);
            }
            for (auto at = chain.rbegin(); at != chain.rend(); ++at) {
                gtk_widget_add_css_class(*at, "va-restyled");
                gtk_widget_remove_css_class(*at, "va-restyled");
                GdkRGBA taken;
                gtk_widget_get_color(*at, &taken);
            }
        }
        inks.push_back(colour_of(area, GdkRGBA{}));
        inks.push_back(colour_of(drawn->now, GdkRGBA{}));
        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, chart_height);
        cairo_t* cr = cairo_create(surface);
        cairo_set_source_rgb(cr, ((ground >> 16) & 0xff) / 255.0, ((ground >> 8) & 0xff) / 255.0, (ground & 0xff) / 255.0);
        cairo_paint(cr);
        draw_chart(GTK_DRAWING_AREA(area), cr, width, chart_height, drawn);
        cairo_destroy(cr);
        cairo_surface_flush(surface);
        // Lines, the fill below them, the axis and the stretch drawn: a good
        // part of the image is not its background.
        const auto* pixels = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        int painted = 0;
        for (int row = 0; pixels && row < chart_height; ++row) {
            const auto* line = reinterpret_cast<const std::uint32_t*>(pixels + static_cast<std::ptrdiff_t>(row) * stride);
            painted += static_cast<int>(
                std::count_if(line, line + width, [ground = ground](std::uint32_t pixel) { return pixel != ground; }));
        }
        if (painted < width * chart_height / 10) {
            failure = std::string("the sample read speed chart drew almost nothing in the ") + name + " style";
        }
        if (folder && *folder) {
            const auto into = path_from_utf8(folder);
            std::error_code error;
            std::filesystem::create_directories(into, error);
            const auto file = path_to_utf8(into / (std::string("sample-chart-") + name + ".png"));
            if (error || cairo_surface_write_to_png(surface, file.c_str()) != CAIRO_STATUS_SUCCESS) {
                failure = "cannot write " + file;
            }
        }
        cairo_surface_destroy(surface);
    }
    adw_style_manager_set_color_scheme(style, scheme);
    gtk_window_destroy(GTK_WINDOW(window));
    // The chart takes its colours from the style: the dark one has others,
    // for its axis and for its line.
    if (failure.empty() &&
        (inks.size() != 4 || gdk_rgba_equal(&inks[0], &inks[2]) || gdk_rgba_equal(&inks[1], &inks[3]))) {
        failure = "the read speed chart draws in the same colours in the light and the dark style";
    }
    if (!failure.empty()) {
        throw std::runtime_error(failure);
    }
}

void check_sample_report(GuiState& state)
{
    // Sample data, made up for this check, of no archive, device or computer
    // (ARCH-SAMPLE, c0ffee00-... ids, SAMPLE-* names): a FULL verification
    // that FAILED on a modified file whose name holds what HTML must escape,
    // whose device warned of its health and which read its content markedly
    // slower from 12% to 25% than the archive's earlier one, which PASSED;
    // the two the only runs of the repository, whose chain was found intact.
    ReadSpeed now_read;
    now_read.content_bytes = 100'000'000'000;
    now_read.devices = {"sample SAMPLE-SERIAL"};
    ReadSpeed before_read = now_read;
    for (std::uint32_t hundredth = 0; hundredth < 100; ++hundredth) {
        now_read.parts.push_back({hundredth, 1'000'000'000, hundredth >= 12 && hundredth <= 24 ? 36'000u : 12'000u});
        before_read.parts.push_back({hundredth, 1'000'000'000, 11'000});
    }
    VerifyRunRecord earlier;
    earlier.run_id = "c0ffee00-0000-4000-9000-0000000000e0";
    earlier.archive_uuid = "c0ffee00-0000-4000-8000-0000000000e1";
    earlier.revision = 1;
    earlier.mode = "FULL";
    earlier.started = "2026-08-04T01:00:00Z";
    earlier.finished = "2026-08-04T01:18:20Z";
    earlier.result = "PASSED";
    earlier.coverage_bytes = now_read.content_bytes;
    earlier.counts_json = nlohmann::json{{"items", {{"Verified", 3}}}, {"read_speed", read_speed_json(before_read)}}.dump();
    earlier.row_hash = "c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee0e";
    auto compared = compare_read_speed(now_read, before_read);
    if (!compared) {
        throw std::runtime_error("the sample reads of the verification report compare with nothing");
    }
    compared->baseline_run = earlier.run_id;
    compared->baseline_finished = earlier.finished;
    PhysicalDeviceInfo device;
    device.model = "SAMPLE-DEVICE";
    device.serial = "SAMPLE-SERIAL";
    device.health = DeviceHealth{HealthStatus::Warning, {"predicting-failure"}};
    app::ReportFacts facts;
    auto& run = facts.recorded.run;
    run.run_id = "c0ffee00-0000-4000-9000-0000000000e1";
    run.archive_uuid = earlier.archive_uuid;
    run.revision = 1;
    run.mode = "FULL";
    run.started = "2026-11-04T01:00:00Z";
    run.finished = "2026-11-04T01:24:30Z";
    run.result = "FAILED";
    run.coverage_bytes = now_read.content_bytes - 1'000'000'000;
    run.counts_json = nlohmann::json{{"items", {{"Modified", 1}, {"MetadataMismatch", 1}, {"Verified", 1}}},
        {"read_speed", read_speed_json(now_read)}, {"read_compared", speed_comparison_json(*compared)},
        {"device_health", device_health_json({device})}}
                          .dump();
    run.host = "SAMPLE-RECORDED-HOST";
    run.app_version = "0.0.0-sample";
    run.row_hash = "c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00";
    VerifyItemRecord item;
    item.run_id = run.run_id;
    item.path.p = "SAMPLE/<script>alert(1)</script> & \"quotes\".txt";
    item.status = "Modified";
    item.detail = "content differs";
    VerifyItemRecord metadata = item;
    metadata.path.p = "SAMPLE/預算表.xlsx";
    metadata.status = "MetadataMismatch";
    metadata.detail = "mtime";
    facts.recorded.items = {metadata, item};
    facts.recorded.history = {earlier, run};
    facts.recorded.label = "ARCH-SAMPLE";
    facts.recorded.volume_label = "SAMPLE-VOLUME";
    facts.runs = {earlier, run};
    facts.labels = {{run.archive_uuid, "ARCH-SAMPLE"}};
    HistoryCheck intact;
    intact.runs = 2;
    facts.chain = intact;
    facts.made = {"2026-11-04T02:00:00Z", "SAMPLE-HOST", state.host.app_version};
    // As Save report... makes it, in the window's language.
    const auto shown = shown_identity(state.identity_shown);
    const auto html = app::run_report_html(state.text, facts, shown);
    const auto lang = std::string("<html lang=\"") + std::string(app::language_code(state.text.language())) + "\">";
    if (!html || !html->starts_with("<!doctype html>\n") || !html->contains(lang) ||
        !html->contains(app::html_escaped(result_with_mode(state.text, run))) || html->contains("<script") ||
        !html->contains("&lt;script&gt;") || !html->contains("<svg class=\"chart\"") ||
        !html->ends_with("</html>\n")) {
        throw std::runtime_error("the sample verification report is not the page it should be");
    }
    auto incomplete = facts;
    incomplete.recorded.run.result = "INCOMPLETE";
    if (app::run_report_html(state.text, incomplete, shown)) {
        throw std::runtime_error("a verification report was made of an INCOMPLETE run");
    }
    // Where VERIARCHIVE_GUI_SMOKE_CHART names a folder, written there as
    // Save report... writes it, to look at.
    if (const char* folder = std::getenv("VERIARCHIVE_GUI_SMOKE_CHART"); folder && *folder) {
        const auto into = path_from_utf8(folder);
        std::error_code error;
        std::filesystem::create_directories(into, error);
        if (const auto failure = write_report(state.text, facts, shown, into / "sample-report.html")) {
            throw std::runtime_error("cannot write the sample verification report: " + failure(app::Text()));
        }
    }
}

} // namespace va::gui
