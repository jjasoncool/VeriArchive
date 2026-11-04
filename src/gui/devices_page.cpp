// Devices (decision 0046): the devices attached now, as the latest read of
// them found them (devices_poll.cpp), each a card: its model, bus and
// capacity, the health it reports of itself (decision 0044), its serial as it
// reports it, and its volumes, each with where it is mounted (or its GUID
// path), its capacity, the space in use and whether it is read-only; on each
// volume the archives whose roots lie there, each of which leads to it in
// Archives, one whose filesystem is there on another device than recorded
// (MIGRATED) said so; or Register this volume, which opens Register Archive
// with it chosen, or, while a registration works on it, its root. Read when
// the page comes on screen, with Refresh, and every 30 seconds while it is
// shown. In the screenshot mode the fixed sample list (sample_volumes), never
// this computer's devices, and the volume of the sample archives' folders as
// the sample identity alone.
#include "gui/gui_app.h"

#include "common/utf8.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace va::gui {
namespace {

// What a row of a card leads to: an archive in Archives, or Register Archive
// with a volume chosen.
struct CardAction {
    GuiState* state;
    std::string target;  // the archive's uuid, or the volume's root
};

GtkWidget* card_row(const std::string& title, const std::string& subtitle)
{
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
    if (!subtitle.empty()) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
    }
    gtk_widget_add_css_class(row, "numeric");
    return row;
}

// The icon of a device by its bus as it reports it: one on USB, or another.
const char* device_icon(const std::string& bus)
{
    std::string lower = bus;
    std::ranges::transform(lower, lower.begin(), [](unsigned char character) { return std::tolower(character); });
    return lower.find("usb") != std::string::npos ? "drive-harddisk-usb-symbolic" : "drive-harddisk-symbolic";
}

// An archive on a volume, a row that selects it in Archives.
GtkWidget* archive_on_volume_row(GuiState& state, const ArchiveOnVolume& archive)
{
    GtkWidget* row = card_row(archive.title, archive.root);
    // Under its volume's row, set in.
    GtkWidget* indent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(indent, 18, -1);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), indent);
    GtkWidget* icon = gtk_image_new_from_icon_name(archive.migrated ? "dialog-warning-symbolic" : "package-x-generic-symbolic");
    gtk_widget_add_css_class(icon, archive.migrated ? "warning" : "dim-label");
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), icon);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_signal_connect_data(row, "activated",
        G_CALLBACK(+[](AdwActionRow*, gpointer data) {
            const auto* action = static_cast<const CardAction*>(data);
            auto& state = *action->state;
            const auto archive = action->target;
            guarded(
                [&] {
                    show_page(state, "archives");
                    select_archive(state, archive, true);
                },
                [&](const Report& report) { show_archives_status(state, report); });
        }),
        new CardAction{&state, archive.archive_uuid},
        +[](gpointer data, GClosure*) { delete static_cast<CardAction*>(data); }, GConnectFlags{});
    return row;
}

// A volume of a device, with Register this volume where it is offered, or
// the root a registration works on there.
GtkWidget* volume_row(GuiState& state, const VolumeLine& volume)
{
    const auto& text = state.text;
    GtkWidget* row = card_row(volume.title, volume.subtitle);
    if (!volume.registering.empty()) {
        GtkWidget* working = gtk_label_new(volume.registering.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(working), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(working), 40);
        gtk_widget_set_tooltip_text(working, volume.registering.c_str());
        gtk_widget_add_css_class(working, "dim-label");
        gtk_widget_set_valign(working, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), working);
    }
    if (!volume.register_root.empty()) {
        GtkWidget* add = gtk_button_new_with_label(text("Register this volume").c_str());
        gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
        // Which volume it registers, also where several have no label.
        gtk_accessible_update_property(GTK_ACCESSIBLE(add), GTK_ACCESSIBLE_PROPERTY_LABEL,
            text.format("{} · {}", text("Register this volume"),
                    text.format("{} · {}", volume.title, volume.register_root))
                .c_str(),
            -1);
        g_signal_connect_data(add, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                const auto* action = static_cast<const CardAction*>(data);
                auto& state = *action->state;
                const auto root = action->target;
                guarded([&] { register_volume(state, root); }, [&](const Report& report) { show_message(state, report); });
            }),
            new CardAction{&state, volume.register_root},
            +[](gpointer data, GClosure*) { delete static_cast<CardAction*>(data); }, GConnectFlags{});
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), add);
    }
    return row;
}

// A device's card: its head (model, bus and capacity, health), its serial,
// then each volume with the archives on it.
GtkWidget* device_card(GuiState& state, const DeviceCard& card)
{
    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    GtkWidget* head = card_row(card.title, card.subtitle);
    gtk_widget_add_css_class(head, "va-device-head");
    GtkWidget* icon = gtk_image_new_from_icon_name(card.health ? device_icon(card.bus) : "dialog-question-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
    adw_action_row_add_prefix(ADW_ACTION_ROW(head), icon);
    if (card.health) {
        GtkWidget* health = make_pill(*card.health);
        gtk_widget_set_valign(health, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(head), health);
    }
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(head), FALSE);
    gtk_list_box_append(GTK_LIST_BOX(list), head);
    if (!card.fields.empty()) {
        gtk_list_box_append(GTK_LIST_BOX(list), make_fields_row(card.fields));
    }
    for (const auto& volume : card.volumes) {
        gtk_list_box_append(GTK_LIST_BOX(list), volume_row(state, volume));
        for (const auto& archive : volume.archives) {
            gtk_list_box_append(GTK_LIST_BOX(list), archive_on_volume_row(state, archive));
        }
    }
    return list;
}

// The roots that registrations work on: those of this window's registration
// jobs that wait or work, and those of the unfinished registrations of the
// repository open now that its latest read of them found working, wherever
// they were started (a stopped one is resumed through Register Archive too).
std::vector<std::filesystem::path> registering_roots(const GuiState& state)
{
    std::vector<std::filesystem::path> roots;
    for (const auto& [job, root] : state.registering) {
        roots.push_back(root);
    }
    if (!state.session || state.operations.repository != state.session->context.repo.root) {
        return roots;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto wall_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    for (const auto& watch : state.operations.watches) {
        const auto& operation = watch.operation();
        if (operation.kind != app::OperationKind::Registration || operation.root.empty()) {
            continue;
        }
        switch (watch.state(now, wall_now)) {
        case app::OperationState::Stopped:
        case app::OperationState::Differs:
        case app::OperationState::LaterRevision:
        case app::OperationState::ArchiveRetired:
            break;
        default:
            roots.push_back(path_from_utf8(operation.root));
            break;
        }
    }
    return roots;
}

} // namespace

void refresh_devices(GuiState& state)
{
    auto& page = state.devices_page;
    if (!page.cards) {
        return;
    }
    const auto& text = state.text;
    const auto& poll = state.devices;
    const auto* read = attached_now(state);
    gtk_widget_set_visible(page.reading, poll.busy);
    gtk_spinner_set_spinning(GTK_SPINNER(page.reading), poll.busy);
    // How many are attached; that they are being read, before the first read
    // is back, or that a device may not be answering, when one takes long.
    const auto reading_for = std::chrono::steady_clock::now() - poll.started;
    std::string status = read ? devices_status(text, *read) : std::string{};
    if (poll.busy && (!read || reading_for >= devices_slow_after)) {
        status = reading_words(text, reading_for);
    }
    gtk_label_set_text(GTK_LABEL(page.status), status.c_str());
    // Why the read failed; or why the archives on the volumes cannot be
    // told, while the devices can.
    std::optional<std::string> problem;
    if (!read && poll.error) {
        problem = poll.error(text);
    } else if (read && read->unread) {
        problem = text.format("Cannot tell which archives are on them: {}", text(*read->unread));
    }
    gtk_label_set_text(GTK_LABEL(page.problem_words), problem ? problem->c_str() : "");
    gtk_widget_set_visible(page.problem, problem.has_value());

    const auto cards = read ? device_cards(text, *read,
                                  archive_names(state.archives_page.statuses, state.identity_shown),
                                  state.identity_shown, registering_roots(state))
                            : std::vector<DeviceCard>{};
    // The same as drawn: the keyboard focus stays where it is.
    if (page.shown.has_value() == (read != nullptr) && (!read || *page.shown == cards)) {
        return;
    }
    // The focus on what goes goes to Refresh.
    if (state.window && page.refresh) {
        GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(state.window));
        if (focus && (focus == page.cards || gtk_widget_is_ancestor(focus, page.cards))) {
            gtk_widget_grab_focus(page.refresh);
        }
    }
    while (GtkWidget* child = gtk_widget_get_first_child(page.cards)) {
        gtk_box_remove(GTK_BOX(page.cards), child);
    }
    for (const auto& card : cards) {
        gtk_box_append(GTK_BOX(page.cards), device_card(state, card));
    }
    page.shown = read ? std::optional(cards) : std::nullopt;
}

GtkWidget* build_devices_page(GuiState& state)
{
    auto& page = state.devices_page;
    page = {};
    const auto& text = state.text;
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    page.page = box;
    set_margins(box, 24);

    // The head: how many devices are attached, that they are being read, and
    // Refresh. The page's name is the header bar's title above it.
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    page.status = make_label("");
    gtk_widget_add_css_class(page.status, "dim-label");
    gtk_widget_add_css_class(page.status, "numeric");
    gtk_widget_set_hexpand(page.status, TRUE);
    gtk_widget_set_valign(page.status, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(head), page.status);
    page.reading = gtk_spinner_new();
    gtk_widget_set_valign(page.reading, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(page.reading, FALSE);
    gtk_box_append(GTK_BOX(head), page.reading);
    page.refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_valign(page.refresh, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(page.refresh, "flat");
    gtk_widget_set_tooltip_text(page.refresh, text("Refresh").c_str());
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(page.refresh), GTK_ACCESSIBLE_PROPERTY_LABEL, text("Refresh").c_str(), -1);
    g_signal_connect(page.refresh, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                read_devices(state, DevicesReadWhy::Refresh);
                // And the unfinished operations, for the volumes that
                // registrations are working on (Archives reads them itself).
                if (state.operations.timer == 0) {
                    start_poll(state);
                }
            },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
    gtk_box_append(GTK_BOX(head), page.refresh);
    gtk_box_append(GTK_BOX(box), head);

    GtkWidget* explanation = make_label(
        text("This page lists the devices attached to this computer now, the health each reports, the volumes on "
             "each, and the archives whose roots lie on those volumes. It reads them again every 30 seconds while "
             "this page or Archives is shown, and at once with Refresh. Reading them needs no administrator "
             "permission, and this page writes nothing to them."));
    gtk_widget_add_css_class(explanation, "dim-label");
    gtk_box_append(GTK_BOX(box), explanation);

    page.problem = make_line_box("va-error", "dialog-warning-symbolic", "error");
    page.problem_words = make_words("", nullptr, true);
    gtk_widget_set_hexpand(page.problem_words, TRUE);
    gtk_box_append(GTK_BOX(page.problem), page.problem_words);
    gtk_widget_set_visible(page.problem, FALSE);
    gtk_box_append(GTK_BOX(box), page.problem);

    page.cards = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_box_append(GTK_BOX(box), page.cards);

    // Read only while the page is on screen, as Archives is.
    g_signal_connect(box, "map", G_CALLBACK(+[](GtkWidget* shown, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { watch_devices(state, shown); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    g_signal_connect(box, "unmap", G_CALLBACK(+[](GtkWidget* hidden, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { stop_watching_devices(state, hidden); }, [](const Report&) {});
    }), &state);

    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 700);
    adw_clamp_set_child(ADW_CLAMP(clamp), box);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);
    gtk_widget_set_vexpand(scroller, TRUE);
    refresh_devices(state);
    return scroller;
}

} // namespace va::gui
