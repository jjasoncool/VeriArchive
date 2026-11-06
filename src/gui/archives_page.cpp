// Archives (§B17, decisions 0022, 0046 and 0049): every archive of the
// repository, beside the details of the one selected (archive_detail.cpp),
// where its verifications start. Never a bare "integrity status" (R-21): each
// row gives the result of the last FULL verification and how long ago it was,
// with OVERDUE, the device alerts of the last verification, a retired archive
// and a manifest that cannot be used. Above the archives, an overview
// (decision 0049): how many archives there are and how many need attention,
// how many are attached and the latest FULL verification; a tile for each
// standing (passed, OVERDUE, never FULL verified, last FAILED, a device
// alert), which shows only those archives in the list; Verify the attached
// archives that need it, Register Archive and Cross-check...; and the way to
// the unfinished operations that stopped, which Jobs lists (decision 0049;
// they were listed here). The unfinished operations are read every few
// seconds while the page is shown (operations_poll.cpp), for the details'
// Resume and the overview. Each row starts with where its archive is attached
// now, or that it is not, as the reads of the attached devices find it
// (devices_poll.cpp), which run while the page is shown too. Without archives,
// a page that leads to the first registration. A narrow window shows the
// list, and the details of an archive in its place. The result page of a run
// opens over the page (result_page.cpp).
#include "gui/gui_app.h"

#include "app/plain_words.h"
#include "core/engine/archive_status.h"
#include "platform/file_system.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace va::gui {
namespace {

// The width of the list beside the details, as the approved design of
// decision 0046 draws it.
constexpr int list_min_width = 280;
constexpr int list_max_width = 400;

const char* archive_of(GtkListBoxRow* row)
{
    return row ? static_cast<const char*>(g_object_get_data(G_OBJECT(row), "va-archive")) : nullptr;
}

// ---- The list of archives ---------------------------------------------------

// An archive's row (archive_row): its status dot, its label and volume label
// with the result beside them, where it is attached now (`attached`, filled
// by show_attached once a read of the attached devices says), the line of
// its carrier and its last FULL verification, its marks, and what is wrong
// with its manifest.
GtkWidget* archive_list_row(const app::Text& text, const ArchiveRow& shown, GuiState::AttachedLineShown& attached)
{
    GtkWidget* row = gtk_list_box_row_new();
    g_object_set_data_full(G_OBJECT(row), "va-archive", g_strdup(shown.archive_uuid.c_str()), g_free);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    GtkWidget* dot = make_dot(shown.tone);
    gtk_widget_set_valign(dot, GTK_ALIGN_START);
    gtk_widget_set_margin_top(dot, 6);
    gtk_box_append(GTK_BOX(box), dot);

    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(words, TRUE);
    GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    // A long label (register --label) is cut short rather than widen the
    // list past its pane and push the result out of view; its tooltip has it
    // whole.
    GtkWidget* label = gtk_label_new(shown.label.c_str());
    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(label, shown.label.c_str());
    gtk_box_append(GTK_BOX(line), label);
    GtkWidget* volume = gtk_label_new(shown.volume_label.c_str());
    gtk_widget_add_css_class(volume, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(volume), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(volume), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(volume, TRUE);
    gtk_box_append(GTK_BOX(line), volume);
    GtkWidget* result = make_pill(shown.result);
    gtk_widget_set_valign(result, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(line), result);
    gtk_box_append(GTK_BOX(words), line);
    // Where it is attached: a long root is cut in its middle, whole in its
    // tooltip; hidden until a read of the attached devices says.
    attached.line = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(attached.line), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(attached.line), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(attached.line, "caption");
    gtk_widget_set_visible(attached.line, FALSE);
    gtk_box_append(GTK_BOX(words), attached.line);
    if (!shown.subtitle.empty()) {
        GtkWidget* subtitle = make_label(shown.subtitle);
        gtk_label_set_wrap_mode(GTK_LABEL(subtitle), PANGO_WRAP_WORD_CHAR);
        gtk_widget_add_css_class(subtitle, "dim-label");
        gtk_widget_add_css_class(subtitle, "caption");
        gtk_widget_add_css_class(subtitle, "numeric");
        gtk_box_append(GTK_BOX(words), subtitle);
    }
    if (!shown.marks.empty()) {
        GtkWidget* marks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        for (const auto& mark : shown.marks) {
            gtk_box_append(GTK_BOX(marks), make_pill(mark));
        }
        gtk_box_append(GTK_BOX(words), marks);
    }
    // Decision 0028: what is wrong with its latest manifest, red but for the
    // lost manifest of a retired archive; selectable in the details.
    if (!shown.problem.empty()) {
        GtkWidget* problem = make_label(shown.problem);
        gtk_label_set_wrap_mode(GTK_LABEL(problem), PANGO_WRAP_WORD_CHAR);
        gtk_widget_add_css_class(problem, "caption");
        gtk_widget_add_css_class(problem, shown.problem_alerts ? "error" : "dim-label");
        gtk_box_append(GTK_BOX(words), problem);
    }
    gtk_box_append(GTK_BOX(box), words);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    // A screen reader reads the row as its words say it.
    std::string said = shown.label;
    for (const auto& part : {shown.volume_label, shown.result.words, shown.subtitle}) {
        if (!part.empty()) {
            said = text.format("{} · {}", said, part);
        }
    }
    for (const auto& mark : shown.marks) {
        said = text.format("{} · {}", said, mark.words);
    }
    if (!shown.problem.empty()) {
        said = text.format("{} · {}", said, shown.problem);
    }
    gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL, said.c_str(), -1);
    attached.row = row;
    attached.said = std::move(said);
    return row;
}

// Gives each row's first line where its archive is attached, as the latest
// read of the attached devices found it, where that changed.
void show_attached_lines(GuiState& state)
{
    auto& page = state.archives_page;
    const auto& text = state.text;
    const auto* read = attached_now(state);
    // The latest read failed: the rows say nothing of where their archives
    // are attached, and the head says why, which Devices tells in full.
    if (page.devices_problem) {
        const bool failed = !read && state.devices.error && state.session;
        gtk_label_set_text(
            GTK_LABEL(page.devices_problem), failed ? text("Cannot read the attached devices; see Devices").c_str() : "");
        gtk_widget_set_visible(page.devices_problem, failed);
    }
    for (auto& [archive, drawn] : page.attached_lines) {
        const auto status = std::ranges::find_if(page.statuses, [&](const ArchiveStatus& status) {
            return status.archive.archive_uuid == archive;
        });
        if (status == page.statuses.end()) {
            continue;
        }
        const auto line = archive_attached(text, read, *status, state.identity_shown).line;
        if (line == drawn.shown) {
            continue;
        }
        drawn.shown = line;
        gtk_label_set_text(GTK_LABEL(drawn.line), line ? line->words.c_str() : "");
        gtk_widget_set_tooltip_text(drawn.line, line && !line->tooltip.empty() ? line->tooltip.c_str() : nullptr);
        gtk_widget_set_visible(drawn.line, line.has_value());
        // Attached: as the row's words; not attached, dim; not as recorded,
        // in the colour of warnings.
        const bool dim = line && line->state == Attached::Nowhere;
        const bool warns = line && (line->state == Attached::Migrated || line->state == Attached::Ambiguous);
        (dim ? gtk_widget_add_css_class : gtk_widget_remove_css_class)(drawn.line, "dim-label");
        (warns ? gtk_widget_add_css_class : gtk_widget_remove_css_class)(drawn.line, "warning");
        // A screen reader reads it after the label, as the row shows it.
        const auto said = line ? text.format("{} · {}", drawn.said, line->words) : drawn.said;
        gtk_accessible_update_property(GTK_ACCESSIBLE(drawn.row), GTK_ACCESSIBLE_PROPERTY_LABEL, said.c_str(), -1);
    }
}

// ---- The overview (decision 0049) ------------------------------------------

const ArchiveStatus* listed_status(const GuiState& state, std::string_view archive)
{
    for (const auto& status : state.archives_page.statuses) {
        if (status.archive.archive_uuid == archive) {
            return &status;
        }
    }
    return nullptr;
}

// The list shows the archives of the filter its tiles chose.
gboolean shows_row(GtkListBoxRow* row, gpointer data)
{
    const auto& state = *static_cast<const GuiState*>(data);
    const char* uuid = archive_of(row);
    const auto* status = uuid ? listed_status(state, uuid) : nullptr;
    return !status || in_filter(*status, state.archive_filter);
}

// Whether a job of the window waits or works on the archive, or one of its
// operations is unfinished (stopped or not): Resume or Abandon... comes
// first.
bool busy_with(const GuiState& state, const Uuid& archive)
{
    if (state.jobs.reads(std::nullopt, archive)) {
        return true;
    }
    return std::ranges::any_of(state.operations.watches, [&](const app::OperationWatch& watch) {
        return watch.operation().archive_uuid == archive;
    });
}

// The archives that "Verify the attached archives that need it" verifies:
// those that need a FULL verification by their state (needs_full_verify),
// attached as recorded as the latest read of the attached devices found
// them, behind no other enclosure (whose verification asks first, decision
// 0047), and not busy (busy_with).
std::vector<Uuid> archives_needing(const GuiState& state)
{
    std::vector<Uuid> needing;
    const auto* read = attached_now(state);
    if (!read) {
        return needing;
    }
    for (const auto& status : state.archives_page.statuses) {
        if (!needs_full_verify(status) || busy_with(state, status.archive.archive_uuid)) {
            continue;
        }
        const auto attached = archive_attached(state.text, read, status, state.identity_shown);
        if (!attached.line || attached.line->state != Attached::Here ||
            enclosure_case(read, status, state.identity_shown)) {
            continue;
        }
        needing.push_back(status.archive.archive_uuid);
    }
    return needing;
}

// The list again with the filter chosen. The archive shown in the details
// stays where the list shows it; otherwise the first one it shows is chosen.
void filter_list(GuiState& state)
{
    auto& page = state.archives_page;
    if (!page.list) {
        return;
    }
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(page.list));
    if (state.archive_selected) {
        if (const auto* status = listed_status(state, *state.archive_selected);
            status && in_filter(*status, state.archive_filter)) {
            return;
        }
    }
    for (const auto& status : page.statuses) {
        if (in_filter(status, state.archive_filter)) {
            select_archive(state, status.archive.archive_uuid, false);
            return;
        }
    }
}

// The tiles shown, or folded away; folded, the list shows every archive.
void show_fold(GuiState& state)
{
    auto& page = state.archives_page;
    if (!page.fold || !page.tiles) {
        return;
    }
    const bool folded = state.overview_folded;
    gtk_widget_set_visible(page.tiles, !folded);
    gtk_button_set_icon_name(GTK_BUTTON(page.fold), folded ? "pan-down-symbolic" : "pan-up-symbolic");
    const auto words = folded ? state.text("Show the tiles") : state.text("Hide the tiles");
    gtk_widget_set_tooltip_text(page.fold, words.c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(page.fold), GTK_ACCESSIBLE_PROPERTY_LABEL, words.c_str(), -1);
    if (folded && state.archive_filter != ArchiveFilter::All) {
        state.archive_filter = ArchiveFilter::All;
        filter_list(state);
        show_overview(state);
    }
}

// Verify the attached archives that need it: a FULL verification of each,
// as FULL verify of its details starts it; those of one device wait for one
// another (decision 0045).
void verify_needing(GuiState& state)
{
    // Read again: a job may have started on one since the overview was shown.
    const auto needing = archives_needing(state);
    std::string names;
    for (const auto& archive : needing) {
        verify_archive(state, archive, VerifyChoice::Full);
        const auto* status = listed_status(state, archive);
        const auto label = status ? display_label(*status) : archive.substr(0, 8);
        names = names.empty() ? label : state.text.format("{}, {}", names, label);
    }
    if (!needing.empty()) {
        show_toast(state,
            needing.size() == 1 ? state.text.format("Started 1 FULL verification: {}", names)
                                : state.text.format("Started {} FULL verifications: {}", needing.size(), names),
            std::nullopt, ToastKind::Started);
    }
    show_overview(state);
}

// A button of the overview with an icon and words.
GtkWidget* overview_button(const char* icon, const std::string& words)
{
    GtkWidget* button = gtk_button_new();
    GtkWidget* face = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(face), icon);
    adw_button_content_set_label(ADW_BUTTON_CONTENT(face), words.c_str());
    // In a narrow window its words are cut short rather than widen it.
    adw_button_content_set_can_shrink(ADW_BUTTON_CONTENT(face), TRUE);
    gtk_button_set_child(GTK_BUTTON(button), face);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    return button;
}

// Puts `child` into the flow box `box`, whose own child around it takes no
// focus: the button in it does.
void add_to_flow(GtkWidget* box, GtkWidget* child)
{
    gtk_flow_box_append(GTK_FLOW_BOX(box), child);
    if (GtkWidget* around = gtk_widget_get_parent(child)) {
        gtk_widget_set_focusable(around, FALSE);
    }
}

GtkWidget* flow_box()
{
    GtkWidget* box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(box), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(box), 8);
    return box;
}

// A tile: how many archives, the dot of its tone and its words; chosen, the
// list shows only those. One tile is chosen at a time, All at first.
GtkWidget* overview_tile(GuiState& state, const OverviewTile& tile, GtkWidget* group_with)
{
    GtkWidget* button = gtk_toggle_button_new();
    gtk_widget_add_css_class(button, "va-tile");
    if (group_with) {
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button), GTK_TOGGLE_BUTTON(group_with));
    }
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* count = gtk_label_new("0");
    gtk_label_set_xalign(GTK_LABEL(count), 0.0f);
    gtk_widget_add_css_class(count, "title-2");
    gtk_widget_add_css_class(count, "numeric");
    gtk_box_append(GTK_BOX(box), count);
    GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* dot = make_dot(tile.tone);
    gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(line), dot);
    GtkWidget* words = gtk_label_new(tile.words.c_str());
    gtk_label_set_xalign(GTK_LABEL(words), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(words), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(words), PANGO_WRAP_WORD_CHAR);
    // Words longer than this wrap, so that the six tiles share one line
    // where the window is wide enough (English words are the longest).
    gtk_label_set_max_width_chars(GTK_LABEL(words), 12);
    gtk_widget_add_css_class(words, "caption");
    gtk_widget_add_css_class(words, "dim-label");
    gtk_box_append(GTK_BOX(line), words);
    gtk_box_append(GTK_BOX(box), line);
    gtk_button_set_child(GTK_BUTTON(button), box);
    g_object_set_data(G_OBJECT(button), "va-count", count);
    g_object_set_data(G_OBJECT(button), "va-filter", GINT_TO_POINTER(static_cast<int>(tile.filter)));
    g_signal_connect(button, "toggled", G_CALLBACK(+[](GtkToggleButton* toggled, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (state.archives_page.overview_filling || !gtk_toggle_button_get_active(toggled)) {
            return;
        }
        guarded(
            [&] {
                state.archive_filter = static_cast<ArchiveFilter>(
                    GPOINTER_TO_INT(g_object_get_data(G_OBJECT(toggled), "va-filter")));
                filter_list(state);
            },
            [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    return button;
}

// The overview's words, tiles and buttons, which show_overview fills.
GtkWidget* build_overview(GuiState& state)
{
    auto& page = state.archives_page;
    const auto& text = state.text;
    GtkWidget* overview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(overview, 24);
    gtk_widget_set_margin_end(overview, 24);
    gtk_widget_set_margin_top(overview, 12);
    gtk_widget_set_margin_bottom(overview, 12);

    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    page.status = make_label("");
    gtk_widget_add_css_class(page.status, "title-4");
    gtk_box_append(GTK_BOX(words), page.status);
    page.overview_line = make_label("");
    gtk_widget_add_css_class(page.overview_line, "dim-label");
    gtk_widget_add_css_class(page.overview_line, "numeric");
    gtk_box_append(GTK_BOX(words), page.overview_line);
    // Shown only while the attached devices cannot be read, when the rows say
    // nothing of where their archives are attached (show_attached_lines).
    page.devices_problem = make_label("");
    gtk_widget_add_css_class(page.devices_problem, "dim-label");
    gtk_widget_set_visible(page.devices_problem, FALSE);
    gtk_box_append(GTK_BOX(words), page.devices_problem);
    // Shown only while a step of retiring an archive runs (decision 0046).
    page.abandoning = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(page.abandoning), TRUE);
    gtk_widget_set_visible(page.abandoning, FALSE);
    gtk_box_append(GTK_BOX(words), page.abandoning);
    gtk_box_append(GTK_BOX(overview), words);

    // The buttons, each as wide as its words; Refresh and the fold at the end.
    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    page.verify_needed = overview_button("media-playback-start-symbolic", "");
    gtk_widget_add_css_class(page.verify_needed, "suggested-action");
    g_signal_connect(page.verify_needed, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { verify_needing(state); }, [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(actions), page.verify_needed);
    GtkWidget* add = overview_button("list-add-symbolic", text("Register Archive"));
    g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { open_register_wizard(state); }, [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(actions), add);
    GtkWidget* cross = overview_button("edit-copy-symbolic", text("Cross-check..."));
    g_signal_connect(cross, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                const auto& statuses = state.archives_page.statuses;
                if (state.archive_selected) {
                    compare_with(state, *state.archive_selected, std::nullopt);
                } else if (!statuses.empty()) {
                    compare_with(state, statuses.front().archive.archive_uuid, std::nullopt);
                }
            },
            [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(actions), cross);
    GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(gap, TRUE);
    gtk_box_append(GTK_BOX(actions), gap);
    GtkWidget* refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    page.refresh = refresh;
    gtk_widget_add_css_class(refresh, "flat");
    gtk_widget_set_tooltip_text(refresh, text("Refresh").c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(refresh), GTK_ACCESSIBLE_PROPERTY_LABEL, text("Refresh").c_str(), -1);
    g_signal_connect(refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                refresh_archives(state);
                start_poll(state);
                read_devices(state, DevicesReadWhy::Refresh);
            },
            [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    gtk_widget_set_valign(refresh, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(actions), refresh);
    page.fold = gtk_button_new();
    gtk_widget_add_css_class(page.fold, "flat");
    g_signal_connect(page.fold, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        state.overview_folded = !state.overview_folded;
        show_fold(state);
    }), &state);
    gtk_widget_set_valign(page.fold, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(actions), page.fold);
    gtk_box_append(GTK_BOX(overview), actions);

    // A tile for each filter, as many on a line as the window's width allows.
    page.tiles = flow_box();
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(page.tiles), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(page.tiles), 2);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(page.tiles), 6);
    page.tile_buttons.clear();
    GtkWidget* first = nullptr;
    for (const auto& tile : overview_tiles(text, {})) {
        GtkWidget* button = overview_tile(state, tile, first);
        first = first ? first : button;
        page.tile_buttons.emplace_back(tile.filter, button);
        add_to_flow(page.tiles, button);
    }
    gtk_box_append(GTK_BOX(overview), page.tiles);

    // The way to the unfinished operations that stopped, in Jobs.
    page.stopped = overview_button("media-playback-pause-symbolic", "");
    gtk_widget_add_css_class(page.stopped, "flat");
    gtk_widget_set_halign(page.stopped, GTK_ALIGN_START);
    gtk_widget_set_visible(page.stopped, FALSE);
    g_signal_connect(page.stopped, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { show_page(state, "jobs"); }, [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(overview), page.stopped);
    show_fold(state);
    return overview;
}

// A narrow window shows the details in place of the list, and the focus goes
// with them; a wide one shows both, and the focus stays in the list.
void show_details_pane(GuiState& state)
{
    auto* split = ADW_NAVIGATION_SPLIT_VIEW(state.archives_page.split);
    if (split && adw_navigation_split_view_get_collapsed(split)) {
        adw_navigation_split_view_set_show_content(split, TRUE);
    }
}

GtkListBoxRow* row_of(GuiState& state, const Uuid& archive)
{
    auto* list = GTK_LIST_BOX(state.archives_page.list);
    for (int index = 0;; ++index) {
        GtkListBoxRow* row = gtk_list_box_get_row_at_index(list, index);
        if (!row) {
            return nullptr;
        }
        if (const char* uuid = archive_of(row); uuid && archive == uuid) {
            return row;
        }
    }
}

// What the page shows where there are no archives to list: none yet, which
// the first registration changes, or no repository open.
void show_empty(GuiState& state, bool repository_open)
{
    auto& page = state.archives_page;
    const auto& text = state.text;
    auto* empty = ADW_STATUS_PAGE(page.empty);
    if (repository_open) {
        adw_status_page_set_icon_name(empty, "package-x-generic-symbolic");
        adw_status_page_set_title(empty, text("No archives yet").c_str());
        set_status_description(page.empty,
            text("Register a folder or volume: VeriArchive records the fingerprints of each of its files. A FULL "
                 "verification later tells whether any of them was modified, corrupted, deleted or added."));
        gtk_button_set_label(GTK_BUTTON(page.empty_button), text("Register the first archive").c_str());
        // Register Archive, a dialog, rather than a page (register_wizard.cpp).
        g_object_set_data(G_OBJECT(page.empty_button), "va-page", nullptr);
    } else {
        // Why none is open (a repository configured whose disk is not
        // attached, say: rule 5), as Settings says it.
        adw_status_page_set_icon_name(empty, "dialog-information-symbolic");
        adw_status_page_set_title(empty, text("No repository is open.").c_str());
        set_status_description(page.empty, state.session_error ? state.session_error(text)
                                                               : text("No repository is open; see Settings."));
        gtk_button_set_label(GTK_BUTTON(page.empty_button), text("Settings").c_str());
        g_object_set_data(G_OBJECT(page.empty_button), "va-page", const_cast<char*>("settings"));
    }
    gtk_stack_set_visible_child_name(GTK_STACK(page.views), "empty");
}

GtkWidget* build_empty(GuiState& state)
{
    auto& page = state.archives_page;
    page.empty = adw_status_page_new();
    page.empty_button = gtk_button_new_with_label("");
    gtk_widget_set_halign(page.empty_button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(page.empty_button, "pill");
    gtk_widget_add_css_class(page.empty_button, "suggested-action");
    g_signal_connect(page.empty_button, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                if (const char* name = static_cast<const char*>(g_object_get_data(G_OBJECT(clicked), "va-page"))) {
                    show_page(state, name);
                } else {
                    open_register_wizard(state);
                }
            },
            [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    adw_status_page_set_child(ADW_STATUS_PAGE(page.empty), page.empty_button);
    return page.empty;
}

// The list beside the details: the list's pane, the details' pane with the
// way back to the list that a narrow window shows above them.
GtkWidget* build_split(GuiState& state)
{
    auto& page = state.archives_page;
    const auto& text = state.text;
    page.list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page.list), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(page.list, "boxed-list");
    gtk_widget_set_valign(page.list, GTK_ALIGN_START);
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(page.list), GTK_ACCESSIBLE_PROPERTY_LABEL, text("Archives").c_str(), -1);
    // The tiles of the overview choose which archives it shows (decision 0049).
    gtk_list_box_set_filter_func(GTK_LIST_BOX(page.list), shows_row, &state, nullptr);
    GtkWidget* none = make_label(text("No archive is in this group. Choose All to see every archive."));
    gtk_widget_add_css_class(none, "dim-label");
    set_margins(none, 18);
    gtk_list_box_set_placeholder(GTK_LIST_BOX(page.list), none);
    // Choosing a row, with a click or the arrow keys, shows its details
    // beside the list; a click or Enter also shows them in place of the list
    // in a narrow window.
    g_signal_connect(page.list, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                const char* uuid = archive_of(row);
                if (state.archives_page.filling || !uuid || state.archive_selected == Uuid(uuid)) {
                    return;
                }
                state.archive_selected = Uuid(uuid);
                show_details(state);
            },
            [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    g_signal_connect(page.list, "row-activated", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow*, gpointer data) {
        show_details_pane(*static_cast<GuiState*>(data));
    }), &state);
    GtkWidget* list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(list_box, 24);
    gtk_widget_set_margin_end(list_box, 12);
    gtk_widget_set_margin_top(list_box, 6);
    gtk_widget_set_margin_bottom(list_box, 24);
    gtk_box_append(GTK_BOX(list_box), page.list);
    GtkWidget* list_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroller), list_box);
    AdwNavigationPage* list_page = adw_navigation_page_new(list_scroller, text("Archives").c_str());

    // The details, and above them the way back to the list in a narrow window.
    GtkWidget* details_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    page.back = gtk_button_new();
    GtkWidget* back_face = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(back_face), "go-previous-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(back_face), text("Archives").c_str());
    gtk_button_set_child(GTK_BUTTON(page.back), back_face);
    gtk_widget_add_css_class(page.back, "flat");
    gtk_widget_set_halign(page.back, GTK_ALIGN_START);
    gtk_widget_set_margin_start(page.back, 18);
    gtk_widget_set_margin_top(page.back, 6);
    gtk_widget_set_visible(page.back, FALSE);
    g_signal_connect(page.back, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(state.archives_page.split), FALSE);
    }), &state);
    gtk_box_append(GTK_BOX(details_box), page.back);
    // The head of the details, which stays above them while they scroll
    // (decision 0049), as wide as they are.
    page.detail_head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(page.detail_head, "va-detail-head");
    gtk_widget_set_margin_start(page.detail_head, 12);
    gtk_widget_set_margin_end(page.detail_head, 24);
    gtk_widget_set_margin_top(page.detail_head, 6);
    gtk_widget_set_margin_bottom(page.detail_head, 8);
    GtkWidget* head_clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(head_clamp), 820);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(head_clamp), 640);
    adw_clamp_set_child(ADW_CLAMP(head_clamp), page.detail_head);
    GtkWidget* head_band = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(head_band, "va-head-band");
    gtk_box_append(GTK_BOX(head_band), head_clamp);
    gtk_box_append(GTK_BOX(details_box), head_band);
    page.detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(page.detail, 12);
    gtk_widget_set_margin_end(page.detail, 24);
    gtk_widget_set_margin_top(page.detail, 6);
    gtk_widget_set_margin_bottom(page.detail, 24);
    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 820);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 640);
    adw_clamp_set_child(ADW_CLAMP(clamp), page.detail);
    GtkWidget* details_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(details_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(details_scroller), clamp);
    gtk_widget_set_vexpand(details_scroller, TRUE);
    gtk_box_append(GTK_BOX(details_box), details_scroller);
    // Scrolled, the head's label is smaller and a line sets it off from the
    // details passing under it.
    g_object_set_data(G_OBJECT(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(details_scroller))), "va-band",
        head_band);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(details_scroller)), "value-changed",
        G_CALLBACK(+[](GtkAdjustment* scrolled, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            auto& page = state.archives_page;
            const bool stuck = gtk_adjustment_get_value(scrolled) > 4;
            if (stuck == page.head_stuck) {
                return;
            }
            page.head_stuck = stuck;
            auto* band = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(scrolled), "va-band"));
            (stuck ? gtk_widget_add_css_class : gtk_widget_remove_css_class)(band, "va-stuck");
            if (page.detail_title) {
                gtk_widget_remove_css_class(page.detail_title, stuck ? "title-1" : "title-3");
                gtk_widget_add_css_class(page.detail_title, stuck ? "title-3" : "title-1");
            }
        }),
        &state);
    page.detail_page = GTK_WIDGET(adw_navigation_page_new(details_box, ""));

    page.split = adw_navigation_split_view_new();
    auto* split = ADW_NAVIGATION_SPLIT_VIEW(page.split);
    gtk_widget_add_css_class(page.split, "va-archives");
    adw_navigation_split_view_set_min_sidebar_width(split, list_min_width);
    adw_navigation_split_view_set_max_sidebar_width(split, list_max_width);
    adw_navigation_split_view_set_sidebar_width_fraction(split, 0.42);
    adw_navigation_split_view_set_sidebar(split, list_page);
    adw_navigation_split_view_set_content(split, ADW_NAVIGATION_PAGE(page.detail_page));
    // A narrow window shows the list or the details, the way back above them.
    set_while_narrow(state, page.split, "collapsed", true);
    set_while_narrow(state, page.back, "visible", true);
    return page.split;
}

} // namespace

GtkWidget* make_pill(const Pill& pill)
{
    GtkWidget* label = gtk_label_new(pill.words.c_str());
    gtk_widget_add_css_class(label, "va-pill");
    gtk_widget_add_css_class(label, tone_class(pill.tone));
    return label;
}

GtkWidget* make_dot(Tone tone)
{
    GtkWidget* dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(dot, "va-dot");
    gtk_widget_add_css_class(dot, tone_class(tone));
    gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
    return dot;
}

bool keep_focus_from(GuiState& state, GtkWidget* part)
{
    auto& page = state.archives_page;
    if (!state.window || !part || !page.refresh) {
        return false;
    }
    GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(state.window));
    if (!focus || (focus != part && !gtk_widget_is_ancestor(focus, part))) {
        return false;
    }
    gtk_widget_grab_focus(page.refresh);
    return true;
}

void show_archives_status(GuiState& state, const Report& report)
{
    if (state.archives_page.status) {
        gtk_label_set_text(GTK_LABEL(state.archives_page.status), report ? report(state.text).c_str() : "");
    }
}

void refresh_archives(GuiState& state)
{
    auto& page = state.archives_page;
    const auto& text = state.text;
    if (!page.list) {
        return;
    }
    show_operations(state);
    // Nothing shown yet: a read of the operations that comes back draws the
    // list again (apply_poll), so one that could not be drawn now is retried.
    const auto shown_before = std::exchange(state.operations.archives_shown, std::string{});
    page.statuses.clear();
    const bool list_focused = keep_focus_from(state, page.list);
    page.filling = true;
    gtk_list_box_remove_all(GTK_LIST_BOX(page.list));
    page.attached_lines.clear();
    page.filling = false;
    gtk_widget_remove_css_class(page.status, "error");
    if (!state.session) {
        gtk_label_set_text(GTK_LABEL(page.status), "");
        show_empty(state, false);
        show_details(state);
        return;
    }
    const auto failed = [&](const std::string& message) {
        gtk_label_set_text(GTK_LABEL(page.status), text(message).c_str());
        gtk_widget_add_css_class(page.status, "error");
        gtk_stack_set_visible_child_name(GTK_STACK(page.views), "archives");
        show_details(state);
    };
    // The list only reads: a database that went missing is reported, not
    // created anew (decision 0022).
    auto store = ArchiveStore::open_read_only(state.session->context.repo);
    if (!store) {
        failed(store.error().message);
        return;
    }
    // The mark before the archives: a change in between is drawn already and
    // only makes the next read draw the list once more, never the other way
    // round.
    const auto mark = store->archive_list_mark();
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto repository_files = platform::make_file_system_access();
    auto statuses = archive_statuses(
        *store, *repository_files, state.session->context.repo, state.session->context.config.policy, now);
    if (!statuses) {
        failed(statuses.error().message);
        return;
    }
    // The archives changed since they were drawn (a new archive, a new
    // revision on another carrier, one abandoned), in this window or
    // elsewhere: where each is attached is told anew while Archives or
    // Devices is shown, after the read in flight if there is one.
    const bool changed = mark && !shown_before.empty() && *mark != shown_before;
    if (mark) {
        state.operations.archives_shown = *mark;
    }
    page.statuses = std::move(*statuses);
    if (changed && state.devices.timer != 0) {
        read_devices(state, DevicesReadWhy::Refresh);
    }
    // The comparison dialog lists the archives listed here, so that the
    // details open it on any of them, also on an archive registered
    // meanwhile from the command line or another window. (History reads
    // them as it is shown.)
    refresh_compare_dialog(state);
    if (page.statuses.empty()) {
        state.archive_selected.reset();
        show_empty(state, true);
        show_details(state);
        return;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(page.views), "archives");

    // The archive selected before, while it is listed; the first otherwise.
    const auto listed = [&](const Uuid& archive) {
        return std::ranges::any_of(
            page.statuses, [&](const ArchiveStatus& status) { return status.archive.archive_uuid == archive; });
    };
    if (!state.archive_selected || !listed(*state.archive_selected)) {
        state.archive_selected = page.statuses.front().archive.archive_uuid;
    }
    page.filling = true;
    for (const auto& status : page.statuses) {
        GtkWidget* row = archive_list_row(text, archive_row(text, status, state.identity_shown, now),
            page.attached_lines[status.archive.archive_uuid]);
        gtk_list_box_append(GTK_LIST_BOX(page.list), row);
        if (status.archive.archive_uuid == *state.archive_selected) {
            gtk_list_box_select_row(GTK_LIST_BOX(page.list), GTK_LIST_BOX_ROW(row));
            if (list_focused) {
                gtk_widget_grab_focus(row);
            }
        }
    }
    page.filling = false;
    filter_list(state);
    // Where each is attached, as the latest read of the attached devices
    // found it; the details say it too.
    show_attached_lines(state);
    show_details(state);
    show_overview(state);
}

void show_overview(GuiState& state)
{
    auto& page = state.archives_page;
    if (!page.status) {
        return;
    }
    const auto& text = state.text;
    const auto& statuses = page.statuses;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    // The heading says how the archives stand, unless the list could not be
    // read, which it says instead (refresh_archives).
    if (!gtk_widget_has_css_class(page.status, "error")) {
        gtk_label_set_text(GTK_LABEL(page.status), state.session ? overview_heading(text, statuses).c_str() : "");
    }
    std::optional<std::size_t> attached;
    if (const auto* read = attached_now(state)) {
        attached = static_cast<std::size_t>(std::ranges::count_if(statuses, [&](const ArchiveStatus& status) {
            const auto shown = archive_attached(text, read, status, state.identity_shown);
            return shown.line && shown.line->state == Attached::Here;
        }));
    }
    const auto line = state.session ? overview_line(text, statuses, attached, now) : std::string{};
    gtk_label_set_text(GTK_LABEL(page.overview_line), line.c_str());
    gtk_widget_set_visible(page.overview_line, !line.empty());

    // The tiles' counts; the one chosen, as the filter says.
    const auto tiles = overview_tiles(text, statuses);
    page.overview_filling = true;
    for (const auto& [filter, button] : page.tile_buttons) {
        const auto tile = std::ranges::find(tiles, filter, &OverviewTile::filter);
        if (tile != tiles.end()) {
            gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(button), "va-count")),
                std::to_string(tile->count).c_str());
            const auto said = text.format("{}: {}", tile->words, tile->count);
            gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, said.c_str(), -1);
        }
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), filter == state.archive_filter);
    }
    page.overview_filling = false;

    // Verify the attached archives that need it, with how many it verifies
    // and their labels in its tooltip.
    page.needing = archives_needing(state);
    if (auto* face = gtk_button_get_child(GTK_BUTTON(page.verify_needed))) {
        adw_button_content_set_label(ADW_BUTTON_CONTENT(face),
            text.format("Verify the attached archives that need it ({})", page.needing.size()).c_str());
    }
    gtk_widget_set_sensitive(page.verify_needed, !page.needing.empty());
    std::vector<std::string> labels;
    for (const auto& archive : page.needing) {
        if (const auto* status = listed_status(state, archive)) {
            labels.push_back(display_label(*status));
        }
    }
    std::string names;
    for (const auto& label : labels) {
        names = names.empty() ? label : text.format("{}, {}", names, label);
    }
    gtk_widget_set_tooltip_text(page.verify_needed,
        labels.empty() ? text("No attached archive is OVERDUE, never FULL verified or last FAILED.").c_str()
                       : text.format("FULL verify {}: OVERDUE, never FULL verified or last FAILED, and attached; those "
                                     "on one device one after another", names)
                             .c_str());

    // The way to the unfinished operations that stopped, in Jobs.
    const auto stopped = stopped_operations(state);
    if (auto* face = gtk_button_get_child(GTK_BUTTON(page.stopped))) {
        adw_button_content_set_label(ADW_BUTTON_CONTENT(face),
            (stopped == 1 ? text("1 unfinished operation has stopped: see Jobs")
                          : text.format("{} unfinished operations have stopped: see Jobs", stopped))
                .c_str());
    }
    gtk_widget_set_visible(page.stopped, stopped > 0);
}

void show_attached(GuiState& state)
{
    if (!state.archives_page.list) {
        return;
    }
    show_attached_lines(state);
    show_details_attached(state);
    show_overview(state);
}

void select_archive(GuiState& state, const Uuid& archive, bool show_content)
{
    auto& page = state.archives_page;
    if (!page.list) {
        return;
    }
    GtkListBoxRow* row = row_of(state, archive);
    if (!row) {
        return;
    }
    if (state.archive_selected != archive) {
        state.archive_selected = archive;
        page.filling = true;
        gtk_list_box_select_row(GTK_LIST_BOX(page.list), row);
        page.filling = false;
        show_details(state);
    }
    if (show_content) {
        show_details_pane(state);
    }
}

GtkWidget* build_archives_page(GuiState& state)
{
    auto& page = state.archives_page;
    page = {};
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    page.page = box;

    gtk_box_append(GTK_BOX(box), build_overview(state));

    page.views = gtk_stack_new();
    gtk_widget_set_vexpand(page.views, TRUE);
    gtk_stack_add_named(GTK_STACK(page.views), build_split(state), "archives");
    gtk_stack_add_named(GTK_STACK(page.views), build_empty(state), "empty");
    gtk_box_append(GTK_BOX(box), page.views);

    // The unfinished operations (for the details' Resume and the overview),
    // and the attached devices, are read only while the page is on screen:
    // "map" when it is shown (also when the
    // window first appears on it), "unmap" when another page is chosen or the
    // window closes, e.g. when a language change replaces it.
    g_signal_connect(box, "map", G_CALLBACK(+[](GtkWidget* shown, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { watch_operations(state, shown); }, [&](const Report& report) { show_poll_error(state, report); });
        guarded([&] { watch_devices(state, shown); }, [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    g_signal_connect(box, "unmap", G_CALLBACK(+[](GtkWidget* hidden, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { stop_watching(state, hidden); }, [](const Report&) {});
        guarded([&] { stop_watching_devices(state, hidden); }, [](const Report&) {});
    }), &state);

    // The page is the first of a navigation view, over which the result page
    // of a run opens (decision 0046); its back button returns here and names
    // the page. Added, not pushed: it stays the first page for good.
    page.root = GTK_WIDGET(adw_navigation_page_new(box, text("Archives").c_str()));
    page.navigation = adw_navigation_view_new();
    adw_navigation_view_add(ADW_NAVIGATION_VIEW(page.navigation), ADW_NAVIGATION_PAGE(page.root));
    return page.navigation;
}

} // namespace va::gui
