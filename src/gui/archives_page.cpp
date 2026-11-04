// Archives (§B17, decisions 0022 and 0046): every archive of the repository,
// beside the details of the one selected (archive_detail.cpp), where its
// verifications start. Never a bare "integrity status" (R-21): each row gives
// the result of the last FULL verification and how long ago it was, with
// OVERDUE, the device alerts of the last verification, a retired archive and
// a manifest that cannot be used. Above the archives, the registrations,
// verifications and relocations that have not finished, with their progress,
// wherever they were started, read every few seconds while the page is shown
// (operations_poll.cpp), with Resume and Abandon... where they have stopped
// (unfinished_actions.cpp). Without archives, a page that leads to the first
// registration. A narrow window shows the list, and the details of an archive
// in its place. The result page of a run opens over the page
// (result_page.cpp).
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

// The most height the operations take before they scroll, so that a long list
// of them never pushes the archives out of the window: about three rows.
constexpr int operations_max_height = 240;

// The width of the list beside the details, as the approved design of
// decision 0046 draws it.
constexpr int list_min_width = 280;
constexpr int list_max_width = 400;

const char* archive_of(GtkListBoxRow* row)
{
    return row ? static_cast<const char*>(g_object_get_data(G_OBJECT(row), "va-archive")) : nullptr;
}

// ---- Unfinished operations --------------------------------------------------

// What a button of a row of the operations does when clicked: `act` on the
// row's operation, named by its key.
struct OperationAction {
    std::string key;
    void (*act)(GuiState&, const std::string&);
};

// A button of a row of the operations; what it does shows in a message of its
// own, or where it brings a job into view.
GtkWidget* operation_button(GuiState& state, const std::string& label, const std::string& key,
    void (*act)(GuiState&, const std::string&))
{
    GtkWidget* button = gtk_button_new_with_label(label.c_str());
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(button), "operation-action", new OperationAction{key, act},
        +[](gpointer data) { delete static_cast<OperationAction*>(data); });
    g_signal_connect(button, "clicked",
        G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            const auto* action =
                static_cast<const OperationAction*>(g_object_get_data(G_OBJECT(clicked), "operation-action"));
            guarded(
                [&] {
                    if (action) {
                        action->act(state, action->key);
                    }
                },
                [&](const Report& report) { show_archives_status(state, report); });
        }),
        &state);
    return button;
}

// A label of an operation's row. Not selectable: a read that finds other
// operations draws the rows anew, which would drop a selection.
GtkWidget* operation_label(const std::string& text, const char* style = nullptr)
{
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    if (style) {
        gtk_widget_add_css_class(label, style);
    }
    return label;
}

// What makes up a row of the operations (OperationShown), besides the words
// and the bar that a read changes in place.
GuiState::OperationShown operation_shape(const app::OperationRow& shown)
{
    GuiState::OperationShown shape;
    shape.key = shown.key;
    shape.offers_resume = shown.offers_resume;
    shape.resume_asks_first = shown.resume_asks_first;
    shape.abandonable = shown.abandonable;
    shape.has_bar = shown.fraction.has_value();
    shape.has_details = !shown.details.empty();
    return shape;
}

bool same_shape(const GuiState::OperationShown& drawn, const GuiState::OperationShown& read)
{
    return drawn.key == read.key && drawn.offers_resume == read.offers_resume &&
           drawn.resume_asks_first == read.resume_asks_first && drawn.abandonable == read.abandonable &&
           drawn.has_bar == read.has_bar && drawn.has_details == read.has_details;
}

// Gives the row `drawn` the words, bar and state of `shown`, the same
// operation as a later read sees it.
void update_operation_row(const app::Text& text, const GuiState::OperationShown& drawn, const app::OperationRow& shown)
{
    const bool stopped = app::has_stopped(shown.state_kind);
    gtk_image_set_from_icon_name(GTK_IMAGE(drawn.icon), stopped ? "media-playback-pause-symbolic" : jobs_icon);
    gtk_label_set_text(GTK_LABEL(drawn.title), text.format("{} · {}", shown.label, shown.kind).c_str());
    gtk_widget_set_tooltip_text(drawn.title, shown.archive_uuid.c_str());
    gtk_label_set_text(GTK_LABEL(drawn.root), shown.root.c_str());
    if (drawn.bar && shown.fraction) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(drawn.bar), *shown.fraction);
    }
    gtk_label_set_text(GTK_LABEL(drawn.progress), shown.progress.c_str());
    if (drawn.details) {
        gtk_label_set_text(GTK_LABEL(drawn.details), shown.details.c_str());
    }
    const bool quiet =
        shown.state_kind == app::OperationState::Quiet || shown.state_kind == app::OperationState::Silent;
    gtk_label_set_text(GTK_LABEL(drawn.state), shown.state.c_str());
    if (stopped) {
        gtk_widget_add_css_class(drawn.state, "warning");
    } else {
        gtk_widget_remove_css_class(drawn.state, "warning");
    }
    if (quiet) {
        gtk_widget_add_css_class(drawn.state, "heading");
    } else {
        gtk_widget_remove_css_class(drawn.state, "heading");
    }
    // A verification's run, and the process the record names; a
    // verification that adds chunk hashes has both (decision 0033).
    std::string tooltip = shown.run_id.empty() ? std::string{} : text.format("Run: {}", shown.run_id);
    if (!shown.process.empty()) {
        tooltip += (tooltip.empty() ? "" : "\n") + shown.process;
    }
    gtk_widget_set_tooltip_text(drawn.state, tooltip.empty() ? nullptr : tooltip.c_str());
}

// One unfinished operation: its archive and kind, its root, its bar where
// the share done is known with what it did, its state, and Resume and
// Abandon... where it offers them. `drawn` keeps the widgets that a later
// read of the same operation changes.
GtkWidget* operation_row(GuiState& state, const app::OperationRow& shown, GuiState::OperationShown& drawn)
{
    const auto& text = state.text;
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);

    drawn.icon = gtk_image_new();
    gtk_widget_set_valign(drawn.icon, GTK_ALIGN_START);
    gtk_widget_add_css_class(drawn.icon, "dim-label");
    gtk_box_append(GTK_BOX(box), drawn.icon);

    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(words, TRUE);
    drawn.title = operation_label("", "heading");
    gtk_box_append(GTK_BOX(words), drawn.title);
    // A long root (a folder deep in a volume) wraps, anywhere in a name,
    // rather than pushing the state and the row's buttons off the window;
    // without the hyphens Pango would add, which would read as part of the
    // path.
    drawn.root = operation_label("", "dim-label");
    PangoAttrList* no_hyphens = pango_attr_list_new();
    pango_attr_list_insert(no_hyphens, pango_attr_insert_hyphens_new(FALSE));
    gtk_label_set_attributes(GTK_LABEL(drawn.root), no_hyphens);
    pango_attr_list_unref(no_hyphens);
    gtk_box_append(GTK_BOX(words), drawn.root);
    if (drawn.has_bar) {
        drawn.bar = gtk_progress_bar_new();
        gtk_widget_set_margin_top(drawn.bar, 4);
        gtk_box_append(GTK_BOX(words), drawn.bar);
    }
    drawn.progress = operation_label("", "numeric");
    gtk_box_append(GTK_BOX(words), drawn.progress);
    if (drawn.has_details) {
        // The details may end with why a total is not known, a whole sentence.
        drawn.details = operation_label("", "dim-label");
        gtk_widget_add_css_class(drawn.details, "caption");
        gtk_box_append(GTK_BOX(words), drawn.details);
    }
    drawn.state = operation_label("");
    gtk_box_append(GTK_BOX(words), drawn.state);
    gtk_box_append(GTK_BOX(box), words);
    update_operation_row(text, drawn, shown);

    // A stopped verification or registration can be resumed here, as on
    // the command line (decision 0022, point 7); a stopped registration
    // (decision 0025), relocation (decision 0026) or verification that
    // adds chunk hashes (decision 0033) can be given up on purpose; and so
    // can an older version's registration that may have stopped
    // (describe_operation), whose Resume asks first.
    if (shown.offers_resume || shown.abandonable) {
        GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_valign(actions, GTK_ALIGN_CENTER);
        if (shown.offers_resume) {
            gtk_box_append(GTK_BOX(actions), operation_button(state, text("Resume"), shown.key,
                                                 shown.resume_asks_first ? ask_to_resume : resume_from_list));
        }
        if (shown.abandonable) {
            GtkWidget* abandon = operation_button(state, text("Abandon..."), shown.key, ask_to_abandon);
            gtk_widget_add_css_class(abandon, "destructive-action");
            gtk_box_append(GTK_BOX(actions), abandon);
        }
        gtk_box_append(GTK_BOX(box), actions);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

GtkWidget* build_operations(GuiState& state)
{
    auto& page = state.archives_page;
    const auto& text = state.text;
    page.operations = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(page.operations, 24);
    gtk_widget_set_margin_end(page.operations, 24);
    gtk_widget_set_margin_bottom(page.operations, 12);
    GtkWidget* heading = gtk_label_new(text("Unfinished operations").c_str());
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(page.operations), heading);
    page.operations_note = make_label("");
    gtk_widget_add_css_class(page.operations_note, "dim-label");
    gtk_box_append(GTK_BOX(page.operations), page.operations_note);
    page.operations_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page.operations_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(page.operations_list, "boxed-list");
    // As tall as its rows up to a limit, then it scrolls: many operations
    // never make the window taller or push the archives out of it.
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), operations_max_height);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), page.operations_list);
    gtk_widget_set_margin_top(scroller, 4);
    gtk_box_append(GTK_BOX(page.operations), scroller);
    gtk_widget_set_visible(page.operations, FALSE);
    return page.operations;
}

// ---- The list of archives ---------------------------------------------------

// An archive's row (archive_row): its status dot, its label and volume label
// with the result beside them, the line of its carrier and its last FULL
// verification, its marks, and what is wrong with its manifest.
GtkWidget* archive_list_row(const app::Text& text, const ArchiveRow& shown)
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
    return row;
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
        g_object_set_data(G_OBJECT(page.empty_button), "va-page", const_cast<char*>("register"));
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

void show_operations(GuiState& state)
{
    auto& page = state.archives_page;
    if (!page.operations) {
        return;
    }
    const auto& text = state.text;
    use_session_repository(state);
    const auto& poll = state.operations;
    show_details_operations(state);
    show_results_operations(state);
    const auto clear = [&] {
        keep_focus_from(state, page.operations_list);
        gtk_list_box_remove_all(GTK_LIST_BOX(page.operations_list));
        page.operation_rows.clear();
    };
    if (!state.session || (poll.watches.empty() && !poll.error)) {
        clear();
        gtk_widget_set_visible(page.operations, FALSE);
        return;
    }
    gtk_widget_set_visible(page.operations, TRUE);
    gtk_label_set_text(GTK_LABEL(page.operations_note),
        poll.error ? poll.error(text).c_str()
                   : text("Registrations, verifications and relocations that have not finished, also those "
                          "started from the command line. The numbers move each time an operation saves its "
                          "progress: after every 1000 files, or once a minute when a file finishes; not while it "
                          "scans.")
                         .c_str());
    const auto now = std::chrono::steady_clock::now();
    const auto wall_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto rows = app::describe_operations(text, poll.watches, now, wall_now);
    // The same operations with the same buttons as drawn: only their words
    // and bars change, so the keyboard focus on a Resume or Abandon... stays
    // (a read every few seconds would take it away otherwise).
    const bool same = rows.size() == page.operation_rows.size() &&
                      std::ranges::equal(page.operation_rows, rows, [](const auto& drawn, const auto& shown) {
                          return same_shape(drawn, operation_shape(shown));
                      });
    if (same) {
        for (std::size_t index = 0; index < rows.size(); ++index) {
            update_operation_row(text, page.operation_rows[index], rows[index]);
        }
    } else {
        clear();
        for (const auto& shown : rows) {
            auto drawn = operation_shape(shown);
            gtk_list_box_append(GTK_LIST_BOX(page.operations_list), operation_row(state, shown, drawn));
            page.operation_rows.push_back(std::move(drawn));
        }
    }
    gtk_widget_set_visible(gtk_widget_get_parent(page.operations_list), !rows.empty());
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
    state.operations.archives_shown.clear();
    page.statuses.clear();
    const bool list_focused = keep_focus_from(state, page.list);
    page.filling = true;
    gtk_list_box_remove_all(GTK_LIST_BOX(page.list));
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
    if (mark) {
        state.operations.archives_shown = *mark;
    }
    page.statuses = std::move(*statuses);
    // Compare Archives lists the archives listed here, so that the details
    // open it on any of them, also on an archive registered meanwhile from
    // the command line or another window. Both lists read archive_statuses,
    // in the same order. (History reads them as it is shown.)
    std::vector<Uuid> uuids;
    for (const auto& status : page.statuses) {
        uuids.push_back(status.archive.archive_uuid);
    }
    if (uuids != state.compare_view.archive_uuids) {
        refresh_archive_choices(state);
    }
    gtk_label_set_text(
        GTK_LABEL(page.status), app::count_of(text, page.statuses.size(), app::Counted::Archive).c_str());
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
        GtkWidget* row = archive_list_row(text, archive_row(text, status, state.identity_shown, now));
        gtk_list_box_append(GTK_LIST_BOX(page.list), row);
        if (status.archive.archive_uuid == *state.archive_selected) {
            gtk_list_box_select_row(GTK_LIST_BOX(page.list), GTK_LIST_BOX_ROW(row));
            if (list_focused) {
                gtk_widget_grab_focus(row);
            }
        }
    }
    page.filling = false;
    show_details(state);
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

    // The head: how many archives, the progress of a step of abandoning,
    // Register Archive and Refresh. The page's name is the header bar's
    // title above it, as on the other pages.
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(head, 24);
    gtk_widget_set_margin_end(head, 24);
    gtk_widget_set_margin_top(head, 12);
    gtk_widget_set_margin_bottom(head, 12);
    page.status = make_label("");
    gtk_widget_add_css_class(page.status, "dim-label");
    gtk_widget_add_css_class(page.status, "numeric");
    gtk_widget_set_hexpand(page.status, TRUE);
    gtk_widget_set_valign(page.status, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(head), page.status);
    // Shown only while a step of abandoning runs (decisions 0025, 0026, 0033).
    page.abandoning = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(page.abandoning), TRUE);
    gtk_widget_set_valign(page.abandoning, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(page.abandoning, 220, -1);
    gtk_widget_set_visible(page.abandoning, FALSE);
    gtk_box_append(GTK_BOX(head), page.abandoning);
    GtkWidget* add = gtk_button_new();
    GtkWidget* add_face = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(add_face), "list-add-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(add_face), text("Register Archive").c_str());
    gtk_button_set_child(GTK_BUTTON(add), add_face);
    gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
    g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { show_page(state, "register"); }, [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(head), add);
    GtkWidget* refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    page.refresh = refresh;
    gtk_widget_set_valign(refresh, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(refresh, "flat");
    gtk_widget_set_tooltip_text(refresh, text("Refresh").c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(refresh), GTK_ACCESSIBLE_PROPERTY_LABEL, text("Refresh").c_str(), -1);
    g_signal_connect(refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                refresh_archives(state);
                start_poll(state);
            },
            [&](const Report& report) { show_archives_status(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(head), refresh);
    gtk_box_append(GTK_BOX(box), head);

    gtk_box_append(GTK_BOX(box), build_operations(state));

    page.views = gtk_stack_new();
    gtk_widget_set_vexpand(page.views, TRUE);
    gtk_stack_add_named(GTK_STACK(page.views), build_split(state), "archives");
    gtk_stack_add_named(GTK_STACK(page.views), build_empty(state), "empty");
    gtk_box_append(GTK_BOX(box), page.views);

    // The unfinished operations are read only while the page is on screen:
    // "map" when it is shown (also when the window first appears on it),
    // "unmap" when another page is chosen or the window closes, e.g. when a
    // language change replaces it.
    g_signal_connect(box, "map", G_CALLBACK(+[](GtkWidget* shown, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { watch_operations(state, shown); }, [&](const Report& report) { show_poll_error(state, report); });
    }), &state);
    g_signal_connect(box, "unmap", G_CALLBACK(+[](GtkWidget* hidden, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { stop_watching(state, hidden); }, [](const Report&) {});
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
