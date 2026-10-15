// Archive List (§B17): never a bare "integrity status", but the last FULL
// verify and the last verify of any mode with dates, plus OVERDUE (R-21).
#include "gui/gui_app.h"

#include "app/local_time.h"
#include "core/engine/archive_status.h"

#include <chrono>

namespace va::gui {
namespace {

std::string run_text(const app::Text& text, const std::optional<VerifyRunRecord>& run, bool with_mode)
{
    if (!run) {
        return text("never");
    }
    return (with_mode ? text(run->mode) + " " : "") + text(run->result) + "\n" + app::local_time(run->finished, text);
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
    const auto& text = state.text;
    if (!view.grid) {
        return;
    }
    clear_grid(view.grid);
    if (!state.session) {
        gtk_label_set_text(GTK_LABEL(view.status), text("No repository is open; see Settings.").c_str());
        return;
    }
    auto store = ArchiveStore::open(state.session->context.repo);
    if (!store) {
        gtk_label_set_text(GTK_LABEL(view.status), text(store.error().message).c_str());
        return;
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto statuses = archive_statuses(*store, state.session->context.config.policy, now);
    if (!statuses) {
        gtk_label_set_text(GTK_LABEL(view.status), text(statuses.error().message).c_str());
        return;
    }
    // Two calls: the compiler checks each literal template against its value.
    const auto count = statuses->size() == 1 ? text.format("{} archive", statuses->size())
                                             : text.format("{} archives", statuses->size());
    gtk_label_set_text(GTK_LABEL(view.status), count.c_str());

    const char* headers[] = {"Label", "UUID", "Volume", "Model", "Capacity", "Last FULL verify",
        "Last verify", "Overdue", "Rev", "Status"};
    for (int column = 0; column < 10; ++column) {
        gtk_grid_attach(GTK_GRID(view.grid), cell(text(headers[column]), true), column, 0, 1, 1);
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
        gtk_grid_attach(GTK_GRID(view.grid), cell(run_text(text, status.last_full, false)), 5, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(run_text(text, status.last_any, true), false, failed), 6, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(overdue ? text("OVERDUE") : "", false, overdue), 7, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell("r" + std::to_string(status.archive.current_revision)), 8, row, 1, 1);
        gtk_grid_attach(GTK_GRID(view.grid), cell(text(status.archive.status)), 9, row, 1, 1);
        ++row;
    }
}

GtkWidget* build_archive_list_view(GuiState& state)
{
    auto& view = state.archive_list;
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(box, 16);
    gtk_box_append(GTK_BOX(box), make_heading(text("Archives")));
    gtk_box_append(GTK_BOX(box), make_label(text(
        "Each archive shows when it was last read in full (FULL verify) and when it was last "
        "verified at all. A result is only as current as its date.")));

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* refresh = gtk_button_new_with_label(text("Refresh").c_str());
    g_signal_connect(refresh, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { refresh_archive_list(state); },
                [&](const Report& report) {
                    gtk_label_set_text(GTK_LABEL(state.archive_list.status), report(state.text).c_str());
                });
        }),
        &state);
    view.status = make_label("");
    // The free width of the row, so the count stays on one line; a long
    // message (an error) still wraps.
    gtk_widget_set_hexpand(view.status, TRUE);
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
