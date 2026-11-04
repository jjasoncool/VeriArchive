// History (§B17, decision 0046): the recorded runs of every archive, newest
// first, or of the archive that the filter names, a page of them at a time
// (history_rows.h). Each row gives the result with its mode, the archive and
// when the run ended, what it read and matched, how long it took and how fast
// it read, and its notes; it opens the run's result page over History. Check
// history chain checks the hash chain that protects the history (R-18,
// R-90) and says in the page what it found and what such a check cannot
// detect. A history that cannot be read is said to be so, never taken for
// none.
#include "gui/gui_app.h"

#include "gui/run_result.h"
#include "platform/file_system.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace va::gui {
namespace {

// The runs listed at first, and each time more are asked for.
constexpr std::size_t runs_at_a_time = 200;

GtkWidget* message_row(const std::string& words, const char* style)
{
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), words.c_str());
    gtk_widget_add_css_class(row, style);
    return row;
}

// A run's row; activated, it opens the run's result page over History.
GtkWidget* run_row(GuiState& state, const HistoryRow& shown)
{
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), shown.title.c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), shown.meta.c_str());
    gtk_widget_add_css_class(row, "numeric");
    GtkWidget* result = make_pill(shown.result);
    gtk_widget_set_valign(result, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), result);
    for (const auto& mark : shown.marks) {
        GtkWidget* pill = make_pill(mark);
        gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), pill);
    }
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "va-run", g_strdup(shown.run_id.c_str()), g_free);
    g_signal_connect(row, "activated", G_CALLBACK(+[](AdwActionRow* activated, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        const std::string run = static_cast<const char*>(g_object_get_data(G_OBJECT(activated), "va-run"));
        guarded([&] { open_result(state, ResultsOver::History, run); },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
    // A screen reader reads the row as its words say it.
    std::string said = state.text.format("{} · {}", shown.result.words, shown.title);
    if (!shown.meta.empty()) {
        said = state.text.format("{} · {}", said, shown.meta);
    }
    for (const auto& mark : shown.marks) {
        said = state.text.format("{} · {}", said, mark.words);
    }
    gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL, said.c_str(), -1);
    return row;
}

// Lists the next page of the filter's runs, then a row that lists more where
// there are more.
void list_more_runs(GuiState& state)
{
    auto& page = state.history_page;
    const auto& text = state.text;
    const auto rows = history_rows(text, page.runs, page.names, state.history_filter, page.listed, runs_at_a_time);
    for (const auto& shown : rows) {
        gtk_list_box_append(GTK_LIST_BOX(page.list), run_row(state, shown));
    }
    page.listed += rows.size();
    const auto total = history_count(page.runs, state.history_filter);
    if (page.listed < total) {
        const auto rest = total - page.listed;
        GtkWidget* more = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(more), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(more),
            text.format("Show {} more runs (of {})", grouped(std::min(rest, runs_at_a_time)), grouped(rest)).c_str());
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(more), TRUE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(more), gtk_image_new_from_icon_name("go-down-symbolic"));
        g_signal_connect(more, "activated", G_CALLBACK(+[](AdwActionRow* activated, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded(
                [&] {
                    // The row goes, and the focus with it to the first run
                    // that it lists.
                    auto* list = GTK_LIST_BOX(state.history_page.list);
                    const auto before = state.history_page.listed;
                    gtk_list_box_remove(list, GTK_WIDGET(activated));
                    list_more_runs(state);
                    if (GtkListBoxRow* first = gtk_list_box_get_row_at_index(list, static_cast<int>(before))) {
                        gtk_widget_grab_focus(GTK_WIDGET(first));
                    }
                },
                [&](const Report& report) { show_message(state, report); });
        }), &state);
        gtk_list_box_append(GTK_LIST_BOX(page.list), more);
    }
}

// The runs of the filter, from the newest, as the last read found them.
void show_runs(GuiState& state)
{
    auto& page = state.history_page;
    if (!page.list) {
        return;
    }
    const auto& text = state.text;
    gtk_list_box_remove_all(GTK_LIST_BOX(page.list));
    page.listed = 0;
    if (!state.session) {
        gtk_list_box_append(GTK_LIST_BOX(page.list), message_row(text("No repository is open; see Settings."), "dim-label"));
        return;
    }
    if (page.unread) {
        gtk_list_box_append(GTK_LIST_BOX(page.list), message_row(page.unread(text), "error"));
        return;
    }
    if (history_count(page.runs, state.history_filter) == 0) {
        gtk_list_box_append(GTK_LIST_BOX(page.list), message_row(no_runs_words(text, state.history_filter), "dim-label"));
        return;
    }
    list_more_runs(state);
}

// The result of the last check of the chain, in the page, worded in the
// language shown; nothing before the first check.
void show_chain(GuiState& state)
{
    auto& page = state.history_page;
    if (!page.chain) {
        return;
    }
    while (GtkWidget* child = gtk_widget_get_first_child(page.chain)) {
        gtk_box_remove(GTK_BOX(page.chain), child);
    }
    page.chain_result = nullptr;
    page.chain_limits = nullptr;
    gtk_widget_set_visible(page.chain, state.history_check.has_value());
    if (!state.history_check) {
        return;
    }
    const auto shown = chain_words(state.text, *state.history_check, page.runs, page.names);
    const bool intact = shown.tone == Tone::Success;
    GtkWidget* line = make_line_box(intact ? "va-success" : "va-error",
        intact ? "object-select-symbolic" : "dialog-warning-symbolic", intact ? "success" : "error");
    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(words, TRUE);
    page.chain_result = make_words(shown.result, "heading", true);
    gtk_box_append(GTK_BOX(words), page.chain_result);
    if (!shown.limits.empty()) {
        page.chain_limits = make_words(shown.limits, "dim-label", true);
        gtk_box_append(GTK_BOX(words), page.chain_limits);
    }
    gtk_box_append(GTK_BOX(line), words);
    gtk_box_append(GTK_BOX(page.chain), line);
}

void check_chain(GuiState& state)
{
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    // Only read: a database that went missing is reported, not created anew.
    auto store = ArchiveStore::open_read_only(state.session->context.repo);
    state.history_check = store ? store->check_history(state.session->context.repository_uuid)
                                : std::expected<HistoryCheck, IoError>(std::unexpected(store.error()));
    show_chain(state);
}

// The filter: every archive, then each archive the last read named.
void fill_filter(GuiState& state)
{
    auto& page = state.history_page;
    const auto& text = state.text;
    std::vector<std::string> items{text("All archives")};
    page.filter_uuids.clear();
    for (const auto& name : page.names) {
        items.push_back(filter_words(text, name));
        page.filter_uuids.push_back(name.archive_uuid);
    }
    std::vector<const char*> pointers;
    for (const auto& item : items) {
        pointers.push_back(item.c_str());
    }
    pointers.push_back(nullptr);
    // An archive no longer listed: every archive's runs.
    if (state.history_filter && std::ranges::find(page.filter_uuids, *state.history_filter) == page.filter_uuids.end()) {
        state.history_filter.reset();
    }
    guint selected = 0;
    if (state.history_filter) {
        selected = static_cast<guint>(std::ranges::find(page.filter_uuids, *state.history_filter) -
                                      page.filter_uuids.begin()) + 1;
    }
    page.filling = true;
    GtkStringList* model = gtk_string_list_new(pointers.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(page.filter), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(page.filter), selected);
    page.filling = false;
}

} // namespace

void refresh_history(GuiState& state)
{
    auto& page = state.history_page;
    if (!page.list) {
        return;
    }
    page.runs.clear();
    page.names.clear();
    page.unread = nullptr;
    if (state.session) {
        const auto cannot = [&](std::string message) {
            page.unread = [message = std::move(message)](const app::Text& text) {
                return text.format("Cannot read the verification history: {}", text(message));
            };
        };
        // Only read: a database that went missing is reported, not created anew.
        if (auto store = ArchiveStore::open_read_only(state.session->context.repo); !store) {
            cannot(store.error().message);
        } else if (auto runs = store->all_verify_runs(); !runs) {
            cannot(runs.error().message);
        } else {
            page.runs = std::move(*runs);
            // A check that found the chain intact says nothing of a run
            // recorded since: shown above the run, it would seem to.
            if (state.history_check && !chain_check_holds(*state.history_check, page.runs.size())) {
                state.history_check.reset();
            }
            // The archives' labels, and the volume labels of their current
            // carriers, which their manifests give, as Archives reads them.
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            const auto repository_files = platform::make_file_system_access();
            const auto statuses = archive_statuses(
                *store, *repository_files, state.session->context.repo, state.session->context.config.policy, now);
            if (!statuses) {
                cannot(statuses.error().message);
            } else {
                page.names = archive_names(*statuses, state.identity_shown);
            }
        }
    }
    fill_filter(state);
    show_runs(state);
    show_chain(state);
}

void show_history_error(GuiState& state, Report report)
{
    state.history_page.unread = std::move(report);
    show_runs(state);
}

void show_history_of(GuiState& state, const Uuid& archive)
{
    state.history_filter = archive;
    const char* shown = state.stack ? gtk_stack_get_visible_child_name(GTK_STACK(state.stack)) : nullptr;
    const bool already = shown && std::string_view(shown) == "history";
    show_page(state, "history");
    // Shown already: showing it reads nothing.
    if (already) {
        refresh_history(state);
    }
}

GtkWidget* build_history_page(GuiState& state)
{
    auto& page = state.history_page;
    page = {};
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 24);

    // The head: the filter beside Check history chain.
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* filter_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(filter_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(filter_list, "boxed-list");
    gtk_widget_set_hexpand(filter_list, TRUE);
    page.filter = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(page.filter), text("Archive").c_str());
    g_signal_connect(page.filter, "notify::selected", G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        auto& page = state.history_page;
        if (page.filling) {
            return;
        }
        guarded(
            [&] {
                const auto selected = adw_combo_row_get_selected(ADW_COMBO_ROW(source));
                state.history_filter = selected >= 1 && selected <= page.filter_uuids.size()
                                           ? std::optional(page.filter_uuids[selected - 1])
                                           : std::nullopt;
                show_runs(state);
            },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
    gtk_list_box_append(GTK_LIST_BOX(filter_list), page.filter);
    gtk_box_append(GTK_BOX(head), filter_list);
    page.check = gtk_button_new_with_label(text("Check history chain").c_str());
    gtk_widget_set_valign(page.check, GTK_ALIGN_CENTER);
    g_signal_connect(page.check, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { check_chain(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(head), page.check);
    gtk_box_append(GTK_BOX(box), head);

    page.chain = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_visible(page.chain, FALSE);
    gtk_box_append(GTK_BOX(box), page.chain);

    page.list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page.list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(page.list, "boxed-list");
    gtk_widget_set_valign(page.list, GTK_ALIGN_START);
    gtk_accessible_update_property(GTK_ACCESSIBLE(page.list), GTK_ACCESSIBLE_PROPERTY_LABEL, text("History").c_str(), -1);
    gtk_box_append(GTK_BOX(box), page.list);

    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 700);
    adw_clamp_set_child(ADW_CLAMP(clamp), box);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
    gtk_widget_set_vexpand(scroller, TRUE);

    // The first page of a navigation view, over which a run's result page
    // opens; its back button returns here and names the page.
    page.root = GTK_WIDGET(adw_navigation_page_new(scroller, text("History").c_str()));
    page.navigation = adw_navigation_view_new();
    adw_navigation_view_add(ADW_NAVIGATION_VIEW(page.navigation), ADW_NAVIGATION_PAGE(page.root));
    return page.navigation;
}

} // namespace va::gui
