// Verification History (§B17): every recorded run of an archive, and a check
// of the hash chain that protects the history (R-18, R-90).
#include "gui/gui_app.h"

namespace va::gui {
namespace {

void show_runs(GuiState& state)
{
    auto& view = state.history_view;
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
    const char* headers[] = {"Finished", "Mode", "Result", "Revision", "Bytes matched", "Failures", "Run"};
    for (int column = 0; column < 7; ++column) {
        GtkWidget* header = make_label(headers[column]);
        gtk_widget_add_css_class(header, "heading");
        gtk_grid_attach(GTK_GRID(view.grid), header, column, 0, 1, 1);
    }
    int row = 1;
    for (auto it = runs->rbegin(); it != runs->rend(); ++it, ++row) {
        const auto& run = *it;
        const std::string cells[] = {run.finished, run.mode, run.result, "r" + std::to_string(run.revision),
            format_bytes(run.coverage_bytes.value_or(0)), run.failure_classes, run.run_id};
        for (int column = 0; column < 7; ++column) {
            gtk_grid_attach(GTK_GRID(view.grid), make_label(cells[column], true), column, row, 1, 1);
        }
    }
    if (runs->empty()) {
        gtk_grid_attach(GTK_GRID(view.grid), make_label("Never verified."), 0, 1, 7, 1);
    }
}

void check_chain(GuiState& state)
{
    auto& view = state.history_view;
    if (!state.session) {
        return;
    }
    auto store = ArchiveStore::open(state.session->context.repo);
    const auto check = store ? store->check_history(state.session->context.repository_uuid)
                             : std::expected<HistoryCheck, IoError>(std::unexpected(store.error()));
    std::string text;
    if (!check) {
        text = "Cannot check the history: " + check.error().message;
    } else if (!check->first_broken_run) {
        text = "History intact: " + std::to_string(check->runs) +
               " runs, hash chain verified. (V1 detects edited or deleted rows; signed checkpoints arrive in V1.1.)";
    } else {
        text = "History BROKEN at run " + *check->first_broken_run + ": " + check->problem;
    }
    gtk_label_set_text(GTK_LABEL(view.chain), text.c_str());
}

} // namespace

GtkWidget* build_history_view(GuiState& state)
{
    auto& view = state.history_view;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading("Verification History"));

    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(row), make_label("Archive:"));
    const char* initial[] = {nullptr};
    view.archive = gtk_drop_down_new_from_strings(initial);
    GtkWidget* show = gtk_button_new_with_label("Show runs");
    g_signal_connect(show, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) { show_runs(*static_cast<GuiState*>(data)); }), &state);
    GtkWidget* chain = gtk_button_new_with_label("Check history chain");
    g_signal_connect(chain, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) { check_chain(*static_cast<GuiState*>(data)); }), &state);
    gtk_box_append(GTK_BOX(row), view.archive);
    gtk_box_append(GTK_BOX(row), show);
    gtk_box_append(GTK_BOX(row), chain);
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
