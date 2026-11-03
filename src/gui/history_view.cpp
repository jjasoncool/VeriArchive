// Verification History (§B17): every recorded run of an archive, and a check
// of the hash chain that protects the history (R-18, R-90).
#include "gui/gui_app.h"

#include "app/device_alerts.h"
#include "app/history.h"
#include "app/local_time.h"
#include "app/plain_words.h"

#include <algorithm>
#include <vector>

namespace va::gui {
namespace {

// Shows `result` below the buttons and keeps it, so a language change can
// show it again in the new language. Also where a History handler shows an
// unexpected error (see guarded).
void show_chain_result(GuiState& state, Report result)
{
    gtk_label_set_text(GTK_LABEL(state.history_view.chain), result(state.text).c_str());
    state.history_view.chain_result = std::move(result);
}

void check_chain(GuiState& state)
{
    if (!state.session) {
        return;
    }
    auto store = ArchiveStore::open(state.session->context.repo);
    const auto check = store ? store->check_history(state.session->context.repository_uuid)
                             : std::expected<HistoryCheck, IoError>(std::unexpected(store.error()));
    if (!check) {
        show_chain_result(state, [message = check.error().message](const app::Text& text) {
            return text.format("Cannot check the history: {}", text(message));
        });
    } else if (!check->first_broken_run) {
        show_chain_result(state, [runs = check->runs](const app::Text& text) {
            // Two calls: the compiler checks each literal template against its value.
            return (runs == 1 ? text.format("History intact: {} verification run, hash chain verified.", runs)
                              : text.format("History intact: {} verification runs, hash chain verified.", runs)) +
                   "\n" + text(app::history_chain_limits);
        });
    } else {
        show_chain_result(state,
            [run = *check->first_broken_run, problem = check->problem](const app::Text& text) {
                return text.format("History BROKEN at run {}: {}", run, text(problem));
            });
    }
}

} // namespace

void show_runs(GuiState& state)
{
    auto& view = state.history_view;
    const auto& text = state.text;
    clear_grid(view.grid);
    const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(view.archive));
    if (!state.session || selected == GTK_INVALID_LIST_POSITION || selected >= view.archive_uuids.size()) {
        return;
    }
    auto store = ArchiveStore::open(state.session->context.repo);
    if (!store) {
        return;
    }
    const auto runs = store->verify_runs(view.archive_uuids[selected]);
    if (!runs) {
        return;
    }
    // The runs of a relocation say so in a last column (decision 0026), and
    // so do the device alerts of a run (decision 0044), shown only for an
    // archive that has such runs.
    std::vector<std::string> notes;
    for (const auto& run : *runs) {
        auto note = app::history_note(text, run);
        for (const auto& mark : app::alert_marks(text, run_alerts(run.counts_json))) {
            note += (note.empty() ? "" : "\n") + mark;
        }
        notes.push_back(std::move(note));
    }
    const int columns = std::ranges::any_of(notes, [](const auto& note) { return !note.empty(); }) ? 8 : 7;
    const char* headers[] = {"Finished", "Mode", "Result", "Revision", "Bytes matched", "Failures", "Run", "Note"};
    for (int column = 0; column < columns; ++column) {
        GtkWidget* header = make_label(text(headers[column]));
        gtk_widget_add_css_class(header, "heading");
        gtk_grid_attach(GTK_GRID(view.grid), header, column, 0, 1, 1);
    }
    int row = 1;
    for (auto index = runs->size(); index-- > 0; ++row) {
        const auto& run = (*runs)[index];
        const std::string cells[] = {app::local_time(run.finished, text), text(run.mode), text(run.result),
            "r" + std::to_string(run.revision), format_bytes(run.coverage_bytes.value_or(0)),
            app::failure_classes_words(text, run.failure_classes), run.run_id, notes[index]};
        for (int column = 0; column < columns; ++column) {
            gtk_grid_attach(GTK_GRID(view.grid), make_label(cells[column], true), column, row, 1, 1);
        }
    }
    if (runs->empty()) {
        gtk_grid_attach(GTK_GRID(view.grid), make_label(text("Never verified.")), 0, 1, 7, 1);
    }
}

GtkWidget* build_history_view(GuiState& state)
{
    auto& view = state.history_view;
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(text("Verification History")));

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(row), make_row_label(text("Archive:")));
    const char* initial[] = {nullptr};
    view.archive = gtk_drop_down_new_from_strings(initial);
    view.show = gtk_button_new_with_label(text("Show runs").c_str());
    g_signal_connect(view.show, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { show_runs(state); }, [&](Report error) { show_chain_result(state, std::move(error)); });
        }),
        &state);
    view.check = gtk_button_new_with_label(text("Check history chain").c_str());
    g_signal_connect(view.check, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { check_chain(state); }, [&](Report error) { show_chain_result(state, std::move(error)); });
        }),
        &state);
    gtk_box_append(GTK_BOX(row), view.archive);
    gtk_box_append(GTK_BOX(row), view.show);
    gtk_box_append(GTK_BOX(row), view.check);
    gtk_box_append(GTK_BOX(box), row);

    view.chain = make_label("");
    gtk_box_append(GTK_BOX(box), view.chain);

    view.grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(view.grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(view.grid), 6);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view.grid);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);
    return box;
}

} // namespace va::gui
