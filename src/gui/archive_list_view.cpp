// Archive List (§B17): never a bare "integrity status", but the last FULL
// verify and the last verify of any mode with dates, plus OVERDUE (R-21).
#include "gui/gui_app.h"

#include "core/engine/archive_status.h"

#include <chrono>

namespace va::gui {
namespace {

std::string run_text(const std::optional<VerifyRunRecord>& run, bool with_mode)
{
    if (!run) {
        return "never";
    }
    return (with_mode ? run->mode + " " : "") + run->result + "\n" + run->finished;
}

GtkWidget* cell(const std::string& text, bool bold = false, bool warning = false)
{
    GtkWidget* label = gtk_label_new(nullptr);
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string markup = escaped;
    g_free(escaped);
    if (bold) {
        markup = "<b>" + markup + "</b>";
    }
    if (warning) {
        markup = "<span foreground=\"#c01c28\"><b>" + markup + "</b></span>";
    }
    gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    return label;
}

} // namespace

void refresh_archive_list(GuiState& state)
{
    auto& view = state.archive_list;
    if (!view.grid) {
        return;
    }
    clear_grid(view.grid);
    if (!state.session) {
        gtk_label_set_text(GTK_LABEL(view.status), "No repository is open; see Settings.");
        return;
    }
    auto store = ArchiveStore::open(state.session->context.repo);
    if (!store) {
        gtk_label_set_text(GTK_LABEL(view.status), store.error().message.c_str());
        return;
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto statuses = archive_statuses(*store, state.session->context.config.policy, now);
    if (!statuses) {
        gtk_label_set_text(GTK_LABEL(view.status), statuses.error().message.c_str());
        return;
    }
    const auto count = std::to_string(statuses->size()) + (statuses->size() == 1 ? " archive" : " archives");
    gtk_label_set_text(GTK_LABEL(view.status), count.c_str());

    const char* headers[] = {"Label", "UUID", "Volume", "Model", "Capacity", "Last FULL verify",
        "Last verify", "Overdue", "Rev", "Status"};
    for (int column = 0; column < 10; ++column) {
        gtk_grid_attach(GTK_GRID(view.grid), cell(headers[column], true), column, 0, 1, 1);
    }
    int row = 1;
    for (const auto& status : *statuses) {
        std::string volume;
        std::string model;
        std::string capacity;
        if (status.header) {
            volume = status.header->identity.volume_label;
            capacity = format_bytes(status.header->identity.capacity_bytes);
            if (!status.header->identity.devices.empty()) {
                model = status.header->identity.devices.front().model;
            }
        }
        const bool overdue = status.full_overdue || status.any_overdue;
        const bool failed = status.last_any && status.last_any->result == "FAILED";
        gtk_grid_attach(GTK_GRID(view.grid), cell(display_label(status), true), 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(status.archive.archive_uuid.substr(0, 8)), 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(volume), 2, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(model), 3, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(capacity), 4, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(run_text(status.last_full, false)), 5, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(run_text(status.last_any, true), false, failed), 6, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(overdue ? "OVERDUE" : "", false, overdue), 7, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell("r" + std::to_string(status.archive.current_revision)), 8, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(status.archive.status), 9, row, 1, 1);
        ++row;
    }
}

GtkWidget* build_archive_list_view(GuiState& state)
{
    auto& view = state.archive_list;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading("Archives"));
    gtk_box_append(GTK_BOX(box), make_label(
        "Each archive shows when it was last read in full (FULL verify) and when it was last "
        "verified at all. A result is only as current as its date."));

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* refresh = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) { refresh_archive_list(*static_cast<GuiState*>(data)); }),
        &state);
    view.status = make_label("");
    gtk_box_append(GTK_BOX(toolbar), refresh);
    gtk_box_append(GTK_BOX(toolbar), view.status);
    gtk_box_append(GTK_BOX(box), toolbar);

    view.grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(view.grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(view.grid), 8);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view.grid);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);
    return box;
}

} // namespace va::gui
