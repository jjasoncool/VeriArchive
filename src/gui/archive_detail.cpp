// The details of an archive in Archives (decision 0046), beside the list: its
// label, volume label and root, with a menu (compare it with another archive,
// create a new revision, copy its UUID); a banner with the result of its last
// FULL verification with its mode, when it ended and how long ago, the last
// verification of another mode, and OVERDUE with the limit that fired (R-21,
// R-105); FULL verify, whose menu holds adding chunk hashes (decision 0033),
// strict metadata and the verification of a folder or volume the operator
// chooses, and View result, the result page of that last FULL verification;
// the device alerts of its last verification (decision 0044); its job as a
// card, with Stop and Details (decision 0045); a stopped verification that
// Resume continues; and its device, its content, its copies and its recent
// verifications, each of which opens its result page. Where it is attached
// now, as the latest read of the attached devices found it
// (devices_poll.cpp): a notice above the banner where it is not attached as
// recorded, the tooltip of FULL verify, and its volume and root in the group
// Device.
#include "gui/gui_app.h"

#include "core/engine/add_chunk_hashes.h"
#include "platform/file_system.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace va::gui {
namespace {

// The recent verifications the details list; History has every one.
constexpr std::size_t recent_limit = 10;

GtkWidget* words_label(const std::string& text, const char* style = nullptr, bool selectable = false)
{
    return make_words(text, style, selectable);
}

GtkWidget* group(const std::string& title)
{
    GtkWidget* group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title.c_str());
    return group;
}

// What an item of a menu of the details does once clicked, for the archive
// shown.
struct MenuAction {
    GuiState* state;
    Uuid archive;
    std::function<void(GuiState&, const Uuid&)> act;
};

// An item of a menu (a popover of flat buttons): its name, the line under it
// (`choice.note`), and whether it can be chosen. Clicked, the popover goes and
// `act` runs.
GtkWidget* menu_item(GuiState& state, GtkWidget* popover, const std::string& name, const Choice& choice,
    const Uuid& archive, std::function<void(GuiState&, const Uuid&)> act, GtkWidget** note_label = nullptr)
{
    GtkWidget* button = gtk_button_new();
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "va-menu-item");
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* title = gtk_label_new(name.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(box), title);
    GtkWidget* note = gtk_label_new(choice.note.c_str());
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(note), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 46);
    gtk_label_set_width_chars(GTK_LABEL(note), 30);
    gtk_widget_add_css_class(note, "dim-label");
    gtk_widget_add_css_class(note, "caption");
    gtk_box_append(GTK_BOX(box), note);
    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_widget_set_sensitive(button, choice.offered);
    if (note_label) {
        *note_label = note;
    }
    g_object_set_data(G_OBJECT(button), "va-popover", popover);
    g_signal_connect_data(button, "clicked",
        G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
            auto* action = static_cast<MenuAction*>(data);
            auto& state = *action->state;
            if (auto* popover = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(clicked), "va-popover"))) {
                gtk_popover_popdown(GTK_POPOVER(popover));
            }
            // A copy: what it runs may make the details again, this item with them.
            const auto act = action->act;
            const auto archive = action->archive;
            guarded([&] { act(state, archive); }, [&](const Report& report) { show_archives_status(state, report); });
        }),
        new MenuAction{&state, archive, std::move(act)},
        +[](gpointer data, GClosure*) { delete static_cast<MenuAction*>(data); }, GConnectFlags{});
    return button;
}

GtkWidget* menu_popover()
{
    GtkWidget* popover = gtk_popover_new();
    gtk_widget_add_css_class(popover, "va-menu");
    // Its right edge under the button's, which is near the window's: it
    // opens into the window, not past it.
    gtk_widget_set_halign(popover, GTK_ALIGN_END);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    return popover;
}

void add_to_menu(GtkWidget* popover, GtkWidget* item)
{
    gtk_box_append(GTK_BOX(gtk_popover_get_child(GTK_POPOVER(popover))), item);
}

// The archive's menu (⋯): compare it with another archive, create a new
// revision, copy its UUID.
GtkWidget* more_menu(GuiState& state, const ArchiveStatus& status)
{
    const auto& text = state.text;
    const auto& uuid = status.archive.archive_uuid;
    GtkWidget* button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "view-more-symbolic");
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    const auto label = text.format("Menu of {}", display_label(status));
    gtk_widget_set_tooltip_text(button, label.c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
    GtkWidget* popover = menu_popover();
    add_to_menu(popover, menu_item(state, popover, text("Compare with another archive..."),
                             compare_choice(text, state.archives_page.statuses.size()), uuid,
                             [](GuiState& state, const Uuid& archive) { compare_with(state, archive, std::nullopt); }));
    // The root its latest revision recorded, read where the attached volumes
    // do not hold the archive elsewhere now.
    const std::string root = status.header ? status.header->root_hint : std::string{};
    add_to_menu(popover, menu_item(state, popover, text("Create a new revision..."), new_revision_choice(text, status),
                             uuid, [root](GuiState& state, const Uuid& archive) {
                                 register_new_revision(state, archive, root);
                             }));
    add_to_menu(popover, menu_item(state, popover, text("Copy archive UUID"), Choice{true, uuid}, uuid,
                             [](GuiState& state, const Uuid& archive) {
                                 if (!state.window) {
                                     return;
                                 }
                                 gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(state.window)),
                                     archive.c_str());
                                 show_toast(state, state.text("Archive UUID copied"));
                             }));
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    return button;
}

// FULL verify, and its menu: adding chunk hashes for an archive without them
// (decision 0033), offered where the rule offers it; strict metadata; a
// folder or volume of the operator's choice. Not offered while the archive's
// job waits or works (`busy`, its card shows): a second verification would
// read its whole device once more.
GtkWidget* verify_button(GuiState& state, const ArchiveStatus& status, bool busy)
{
    auto& page = state.archives_page;
    const auto& text = state.text;
    const auto& uuid = status.archive.archive_uuid;
    GtkWidget* button = adw_split_button_new();
    page.verify = button;
    adw_split_button_set_label(ADW_SPLIT_BUTTON(button), text("FULL verify").c_str());
    gtk_widget_add_css_class(button, "suggested-action");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(button), text("Other ways to verify").c_str());
    g_signal_connect_data(button, "clicked",
        G_CALLBACK(+[](AdwSplitButton*, gpointer data) {
            auto* shown = static_cast<MenuAction*>(data);
            auto& state = *shown->state;
            const auto archive = shown->archive;
            guarded([&] { verify_archive(state, archive, VerifyChoice::Full); },
                [&](const Report& report) { show_archives_status(state, report); });
        }),
        new MenuAction{&state, uuid, {}}, +[](gpointer data, GClosure*) { delete static_cast<MenuAction*>(data); },
        GConnectFlags{});

    GtkWidget* popover = menu_popover();
    page.verify_menu = popover;
    if (lists_adding_chunk_hashes(status)) {
        page.adding = menu_item(state, popover, text("FULL verify, and add chunk hashes"),
            adding_choice(text, page.adding_refused, false), uuid,
            [](GuiState& state, const Uuid& archive) {
                verify_archive(state, archive, VerifyChoice::AddingChunkHashes);
            },
            &page.adding_note);
        add_to_menu(popover, page.adding);
    }
    add_to_menu(popover, menu_item(state, popover, text("FULL verify with strict metadata"),
                             Choice{true, text("Timestamp and attribute differences fail the run")}, uuid,
                             [](GuiState& state, const Uuid& archive) {
                                 verify_archive(state, archive, VerifyChoice::StrictMetadata);
                             }));
    add_to_menu(popover, menu_item(state, popover, text("Verify a folder or volume..."),
                             Choice{true, text("Verifies the archive on the folder or volume you choose, for when "
                                               "more than one attached volume holds this archive")},
                             uuid, [](GuiState& state, const Uuid& archive) { choose_and_verify_root(state, archive); }));
    adw_split_button_set_popover(ADW_SPLIT_BUTTON(button), GTK_POPOVER(popover));
    if (busy) {
        gtk_widget_set_sensitive(button, FALSE);
        gtk_widget_set_tooltip_text(button, archive_has_a_job(text, display_label(status)).c_str());
    }
    return button;
}

// The result of the last FULL verification (result_banner), with FULL verify,
// not offered while the archive's job waits or works (`busy`), and View
// result, which opens the result page of that verification.
GtkWidget* banner(GuiState& state, const ArchiveStatus& status, bool busy)
{
    const auto& text = state.text;
    const auto shown = result_banner(text, status, state.session->context.config.policy,
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(box, "va-banner");
    gtk_widget_add_css_class(box, tone_class(shown.tone));
    // The icon on a circle of the tone's colour, which the style draws
    // around it (.va-banner-icon), as large as it is wide.
    GtkWidget* icon = gtk_image_new_from_icon_name(tone_icon(shown.tone));
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 22);
    gtk_widget_add_css_class(icon, "va-banner-icon");
    gtk_widget_add_css_class(icon, tone_class(shown.tone));
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), icon);

    GtkWidget* words = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(words, TRUE);
    gtk_widget_set_valign(words, GTK_ALIGN_CENTER);
    GtkWidget* heading_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* heading = words_label(shown.heading, "title-4");
    gtk_widget_set_hexpand(heading, FALSE);
    gtk_box_append(GTK_BOX(heading_line), heading);
    if (shown.overdue) {
        GtkWidget* overdue = make_pill({text("OVERDUE"), Tone::Warning});
        gtk_widget_set_valign(overdue, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(heading_line), overdue);
    }
    gtk_box_append(GTK_BOX(words), heading_line);
    for (const auto& line : shown.lines) {
        GtkWidget* label = words_label(line, "dim-label");
        gtk_widget_add_css_class(label, "numeric");
        gtk_box_append(GTK_BOX(words), label);
    }
    gtk_box_append(GTK_BOX(box), words);
    // One above the other, as wide as each other: side by side they would
    // leave the words beside them a narrow column in the details' pane.
    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(actions, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(actions), verify_button(state, status, busy));
    if (status.last_full) {
        GtkWidget* view = gtk_button_new_with_label(text("View result").c_str());
        state.archives_page.view_result = view;
        g_object_set_data_full(G_OBJECT(view), "va-run", g_strdup(status.last_full->run_id.c_str()), g_free);
        g_signal_connect(view, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            const std::string run = static_cast<const char*>(g_object_get_data(G_OBJECT(clicked), "va-run"));
            guarded([&] { open_result(state, ResultsOver::Archives, run); },
                [&](const Report& report) { show_archives_status(state, report); });
        }), &state);
        gtk_box_append(GTK_BOX(actions), view);
    }
    gtk_box_append(GTK_BOX(box), actions);
    return box;
}

GtkWidget* line_box(const char* kind, const char* icon_name, const char* icon_style)
{
    return make_line_box(kind, icon_name, icon_style);
}

// A group of fields: each name beside its value, in one row of a boxed list.
GtkWidget* fields_group(const std::string& title, const std::vector<Field>& fields)
{
    GtkWidget* shown = group(title);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown), make_fields_row(fields));
    return shown;
}

// A button of a row of the details that runs `act` for the archive shown.
GtkWidget* row_button(GuiState& state, const std::string& label, const Uuid& archive,
    std::function<void(GuiState&, const Uuid&)> act)
{
    GtkWidget* button = gtk_button_new_with_label(label.c_str());
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_signal_connect_data(button, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* action = static_cast<MenuAction*>(data);
            auto& state = *action->state;
            const auto act = action->act;
            const auto archive = action->archive;
            guarded([&] { act(state, archive); }, [&](const Report& report) { show_archives_status(state, report); });
        }),
        new MenuAction{&state, archive, std::move(act)},
        +[](gpointer data, GClosure*) { delete static_cast<MenuAction*>(data); }, GConnectFlags{});
    return button;
}

GtkWidget* copies_group(GuiState& state, const std::vector<CopyLink>& links)
{
    const auto& text = state.text;
    GtkWidget* shown = group(text("Copies"));
    for (const auto& link : links) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
            link.volume_label.empty() ? link.label.c_str() : text.format("{} · {}", link.label, link.volume_label).c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), link.how.c_str());
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("edit-copy-symbolic"));
        if (link.listed) {
            adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                row_button(state, text("Compare..."), link.copy, [reference = link.reference](GuiState& state, const Uuid& copy) {
                    compare_with(state, reference, copy);
                }));
        }
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown), row);
    }
    return shown;
}

// The archive's runs, newest first; a row opens the run's result page over
// Archives, and a last row History with the archive's runs, where it has more
// than the group lists. Runs that cannot be read are said to be so, never
// taken for none.
GtkWidget* runs_group(GuiState& state, const Uuid& archive)
{
    const auto& text = state.text;
    GtkWidget* shown = group(text("Recent verifications"));
    std::vector<VerifyRunRecord> runs;
    std::optional<std::string> unread;  // why the runs could not be read
    if (auto store = ArchiveStore::open_read_only(state.session->context.repo); !store) {
        unread = store.error().message;
    } else if (auto read = store->verify_runs(archive); !read) {
        unread = read.error().message;
    } else {
        runs = std::move(*read);
    }
    const auto rows = recent_runs(text, runs, recent_limit);
    // A row opens `act` for the archive, or for the run its "va-run" names.
    const auto connect = [&](GtkWidget* row, std::function<void(GuiState&, const Uuid&)> act) {
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_signal_connect_data(row, "activated",
            G_CALLBACK(+[](AdwActionRow*, gpointer data) {
                auto* action = static_cast<MenuAction*>(data);
                auto& state = *action->state;
                // Copies: what it runs may make the details again, this row with them.
                const auto archive = action->archive;
                const auto act = action->act;
                guarded([&] { act(state, archive); }, [&](const Report& report) { show_archives_status(state, report); });
            }),
            new MenuAction{&state, archive, std::move(act)},
            +[](gpointer data, GClosure*) { delete static_cast<MenuAction*>(data); }, GConnectFlags{});
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
    };
    for (const auto& run : rows) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), run.title.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), run.meta.c_str());
        GtkWidget* result = make_pill(run.result);
        gtk_widget_set_valign(result, GTK_ALIGN_CENTER);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), result);
        for (const auto& mark : run.marks) {
            GtkWidget* pill = make_pill(mark);
            gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), pill);
        }
        gtk_widget_add_css_class(row, "numeric");
        connect(row, [run = run.run_id](GuiState& state, const Uuid&) {
            open_result(state, ResultsOver::Archives, run);
        });
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown), row);
    }
    if (runs.size() > rows.size()) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
            text.format("Every verification of this archive in History ({})", runs.size()).c_str());
        connect(row, [](GuiState& state, const Uuid& archive) { show_history_of(state, archive); });
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown), row);
    }
    if (unread) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
            text.format("Cannot read the verifications of this archive: {}", text(*unread)).c_str());
        gtk_widget_add_css_class(row, "error");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown), row);
    } else if (runs.empty()) {
        GtkWidget* row = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), text("Never verified.").c_str());
        gtk_widget_add_css_class(row, "dim-label");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(shown), row);
    }
    return shown;
}

// The archive's job (decision 0045), while it waits or works: the last
// verification started from its details or by Resume of its stopped one.
const JobShown* active_job_of(const GuiState& state, const Uuid& archive)
{
    const auto job = state.verification_jobs.find(archive);
    if (job == state.verification_jobs.end()) {
        return nullptr;
    }
    const auto* shown = state.jobs.find(job->second);
    return shown && shown->state != JobState::Ended ? shown : nullptr;
}

} // namespace

GtkWidget* make_words(const std::string& text, const char* style, bool selectable)
{
    GtkWidget* label = make_label(text, selectable);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    if (style) {
        gtk_widget_add_css_class(label, style);
    }
    return label;
}

const char* tone_icon(Tone tone)
{
    switch (tone) {
    case Tone::Success:
        return "object-select-symbolic";
    case Tone::Warning:
        return "dialog-warning-symbolic";
    case Tone::Error:
        return "dialog-error-symbolic";
    case Tone::Neutral:
        break;
    }
    return "package-x-generic-symbolic";
}

GtkWidget* make_line_box(const char* kind, const char* icon_name, const char* icon_style)
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(box, "va-line");
    gtk_widget_add_css_class(box, kind);
    GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_widget_set_margin_top(icon, 2);
    if (icon_style) {
        gtk_widget_add_css_class(icon, icon_style);
    }
    gtk_box_append(GTK_BOX(box), icon);
    return box;
}

GtkWidget* make_fields_row(const std::vector<Field>& fields)
{
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 16);
    gtk_widget_set_margin_end(grid, 16);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 12);
    int line = 0;
    for (const auto& field : fields) {
        // Up to about 12 characters of Chinese on a line ("上次完整驗證的讀取速度").
        GtkWidget* name = make_words(field.name, "dim-label");
        gtk_label_set_max_width_chars(GTK_LABEL(name), 24);
        gtk_widget_set_valign(name, GTK_ALIGN_START);
        GtkWidget* value = make_words(field.value, nullptr, true);
        // A path fills its lines, broken at any character; and a value is
        // data (a root, a serial, a uuid): wrapped without the hyphens Pango
        // would add, which would read as part of it.
        if (field.path) {
            gtk_label_set_wrap_mode(GTK_LABEL(value), PANGO_WRAP_CHAR);
        }
        PangoAttrList* no_hyphens = pango_attr_list_new();
        pango_attr_list_insert(no_hyphens, pango_attr_insert_hyphens_new(FALSE));
        gtk_label_set_attributes(GTK_LABEL(value), no_hyphens);
        pango_attr_list_unref(no_hyphens);
        gtk_widget_set_hexpand(value, TRUE);
        gtk_widget_set_valign(value, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), name, 0, line, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), value, 1, line, 1, 1);
        ++line;
    }
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), grid);
    return row;
}

void show_details(GuiState& state)
{
    auto& page = state.archives_page;
    if (!page.detail) {
        return;
    }
    const auto& text = state.text;
    // The focus on what goes (FULL verify, a card's Stop) goes to FULL verify
    // of the details made anew.
    const bool focused = keep_focus_from(state, page.detail);
    while (GtkWidget* child = gtk_widget_get_first_child(page.detail)) {
        gtk_box_remove(GTK_BOX(page.detail), child);
    }
    page.card.rows.clear();
    page.notice = nullptr;
    page.notice_key.reset();
    page.notice_words = nullptr;
    page.verify = nullptr;
    page.view_result = nullptr;
    page.verify_menu = nullptr;
    page.adding = nullptr;
    page.adding_note = nullptr;
    page.attached_notice = nullptr;
    page.attached_words = nullptr;
    page.device_group = nullptr;
    page.device_row = nullptr;
    page.device_fields.clear();
    page.attached_shown.reset();
    page.verify_busy = false;
    const auto found = std::ranges::find_if(page.statuses, [&](const ArchiveStatus& status) {
        return state.archive_selected && status.archive.archive_uuid == *state.archive_selected;
    });
    if (!state.session || found == page.statuses.end()) {
        adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(page.detail_page), text("Archives").c_str());
        return;
    }
    const auto& status = *found;
    const auto& uuid = status.archive.archive_uuid;
    const auto label = display_label(status);
    adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(page.detail_page), label.c_str());
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto row = archive_row(text, status, state.identity_shown, now);

    // The rule by which a verification adds chunk hashes (decision 0033),
    // read from the archive's current manifest.
    page.adding_refused = std::optional<std::string>{};
    if (auto store = ArchiveStore::open_read_only(state.session->context.repo)) {
        const auto repository_files = platform::make_file_system_access();
        page.adding_refused =
            why_chunk_hashes_cannot_be_added(*store, *repository_files, state.session->context.repo, status.archive);
    } else {
        page.adding_refused = std::unexpected(store.error());
    }

    // The head: the label, the volume label and root, the menu. Not
    // selectable: the first of the details to take the focus would show its
    // words selected; the content's root and archive UUID can be copied.
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* title_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* title = words_label(label, "title-1");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_box_append(GTK_BOX(title_line), title);
    gtk_box_append(GTK_BOX(title_line), more_menu(state, status));
    gtk_box_append(GTK_BOX(head), title_line);
    std::string where = row.volume_label;
    if (status.header && !status.header->root_hint.empty()) {
        where = where.empty() ? status.header->root_hint : text.format("{} · {}", where, status.header->root_hint);
    }
    if (!where.empty()) {
        gtk_box_append(GTK_BOX(head), words_label(where, "dim-label"));
    }
    gtk_box_append(GTK_BOX(page.detail), head);

    // Decision 0028: what is wrong with its latest manifest, and how to fix
    // it; the manifest's path can be copied.
    if (!row.problem.empty()) {
        gtk_box_append(GTK_BOX(page.detail), words_label(row.problem, row.problem_alerts ? "error" : "dim-label", true));
    }

    // Where the archive is not attached as recorded, from the latest read of
    // the attached devices (show_details_attached fills it).
    page.attached_notice = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_visible(page.attached_notice, FALSE);
    gtk_box_append(GTK_BOX(page.detail), page.attached_notice);

    const auto* job = active_job_of(state, uuid);
    page.verify_busy = job != nullptr;
    gtk_box_append(GTK_BOX(page.detail), banner(state, status, job != nullptr));

    // Decision 0044: the device alerts of its last verification; a slower
    // read leads to the result page of its run, whose chart shows where.
    for (const auto& line : device_alert_lines(text, status, state.identity_shown)) {
        GtkWidget* alert = line_box("va-error", "dialog-warning-symbolic", "error");
        GtkWidget* words = words_label(line.words, nullptr, true);
        gtk_widget_set_hexpand(words, TRUE);
        gtk_box_append(GTK_BOX(alert), words);
        if (!line.slower_run.empty()) {
            GtkWidget* where = row_button(state, text("See where it slowed"), uuid,
                [run = line.slower_run](GuiState& state, const Uuid&) { open_result(state, ResultsOver::Archives, run); });
            gtk_widget_add_css_class(where, "flat");
            gtk_box_append(GTK_BOX(alert), where);
        }
        gtk_box_append(GTK_BOX(page.detail), alert);
    }

    // Its job while it waits or works, as a card that job_changed keeps up
    // to date (decision 0045).
    if (job) {
        GtkWidget* card = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(card), GTK_SELECTION_NONE);
        gtk_widget_add_css_class(card, "boxed-list");
        gtk_list_box_append(GTK_LIST_BOX(card), make_job_card(state, page.card, *job));
        gtk_box_append(GTK_BOX(page.detail), card);
    }

    // A stopped verification of the archive, from the latest read, while
    // the archive has no job.
    page.notice = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_visible(page.notice, FALSE);
    gtk_box_append(GTK_BOX(page.detail), page.notice);

    // Its one row of fields is made again with where the archive is attached
    // (show_details_attached).
    if (auto fields = device_fields(text, status, state.identity_shown); !fields.empty()) {
        page.device_group = group(text("Device"));
        page.device_row = make_fields_row(fields);
        page.device_fields = std::move(fields);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(page.device_group), page.device_row);
        gtk_box_append(GTK_BOX(page.detail), page.device_group);
    }
    GtkWidget* content = fields_group(text("Content"), content_fields(text, status));
    if (status.header && status.header->chunk_size == 0 && page.adding_refused && !*page.adding_refused) {
        adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(content),
            text("The next FULL verification can add chunk hashes (in the menu beside FULL verify), so that a "
                 "verification can tell which part of a large file differs.")
                .c_str());
    }
    gtk_box_append(GTK_BOX(page.detail), content);
    if (const auto links = copy_links(text, status, page.statuses, state.identity_shown); !links.empty()) {
        gtk_box_append(GTK_BOX(page.detail), copies_group(state, links));
    }
    gtk_box_append(GTK_BOX(page.detail), runs_group(state, uuid));
    show_details_operations(state);
    show_details_attached(state);
    if (focused && page.verify) {
        gtk_widget_grab_focus(page.verify);
    }
}

void show_details_attached(GuiState& state)
{
    auto& page = state.archives_page;
    if (!page.attached_notice || !state.archive_selected) {
        return;
    }
    const auto found = std::ranges::find_if(page.statuses, [&](const ArchiveStatus& status) {
        return status.archive.archive_uuid == *state.archive_selected;
    });
    if (found == page.statuses.end()) {
        return;
    }
    const auto& text = state.text;
    auto shown = archive_attached(text, attached_now(state), *found, state.identity_shown);
    // As shown already: the keyboard focus on its words stays.
    if (page.attached_shown == shown) {
        return;
    }

    // The notice: not attached, dim; not as recorded, in the colour of
    // warnings, which FULL verify by the archive's UUID then refuses.
    keep_focus_from(state, page.attached_notice);
    while (GtkWidget* child = gtk_widget_get_first_child(page.attached_notice)) {
        gtk_box_remove(GTK_BOX(page.attached_notice), child);
    }
    page.attached_words = nullptr;
    if (shown.notice && shown.line) {
        const bool here_not = shown.line->state == Attached::Nowhere;
        GtkWidget* notice = here_not ? line_box("va-neutral", "drive-harddisk-symbolic", "dim-label")
                                     : line_box("va-warning", "dialog-warning-symbolic", "warning");
        page.attached_words = words_label(*shown.notice, nullptr, true);
        // It holds a command to copy, with a uuid and a root, which a hyphen
        // that Pango adds where it breaks a line would read as part of.
        PangoAttrList* no_hyphens = pango_attr_list_new();
        pango_attr_list_insert(no_hyphens, pango_attr_insert_hyphens_new(FALSE));
        gtk_label_set_attributes(GTK_LABEL(page.attached_words), no_hyphens);
        pango_attr_list_unref(no_hyphens);
        gtk_widget_set_hexpand(page.attached_words, TRUE);
        gtk_box_append(GTK_BOX(notice), page.attached_words);
        gtk_box_append(GTK_BOX(page.attached_notice), notice);
    }
    gtk_widget_set_visible(page.attached_notice, shown.notice.has_value());

    // FULL verify stays offered: the read may be old, and the verification
    // looks for the archive itself, refusing as it refuses. Its tooltip says
    // what the read found, unless the archive's job holds it.
    if (page.verify && !page.verify_busy) {
        gtk_widget_set_tooltip_text(page.verify, shown.verify_tooltip ? shown.verify_tooltip->c_str() : nullptr);
    }

    // Where it is attached, under its carrier as recorded in the group
    // Device, each name beside its value as theirs.
    if (page.device_group && page.device_row) {
        auto fields = page.device_fields;
        fields.insert(fields.end(), shown.fields.begin(), shown.fields.end());
        keep_focus_from(state, page.device_row);
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(page.device_group), page.device_row);
        page.device_row = make_fields_row(fields);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(page.device_group), page.device_row);
    }
    page.attached_shown = std::move(shown);
}

void show_details_operations(GuiState& state)
{
    auto& page = state.archives_page;
    if (!page.notice) {
        return;
    }
    const auto& text = state.text;
    if (!state.archive_selected) {
        return;
    }
    const auto& archive = *state.archive_selected;
    const auto shown = details_operations(text, state.operations.watches, archive, std::chrono::steady_clock::now(),
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    // Decision 0033: no second verification that adds them while one is unfinished.
    if (page.adding) {
        const auto choice = adding_choice(text, page.adding_refused, shown.adding_unfinished);
        gtk_widget_set_sensitive(page.adding, choice.offered);
        gtk_label_set_text(GTK_LABEL(page.adding_note), choice.note.c_str());
    }

    // The notice while the archive has no job (whose card says how far it
    // is). The same stopped verification as before only has its words
    // changed, so that the keyboard focus on its Resume stays.
    const auto stopped = shown.stopped && !active_job_of(state, archive) ? std::optional(shown.stopped->key)
                                                                         : std::nullopt;
    if (stopped == page.notice_key) {
        if (stopped && page.notice_words) {
            gtk_label_set_text(GTK_LABEL(page.notice_words), shown.words.c_str());
        }
        return;
    }
    keep_focus_from(state, page.notice);
    while (GtkWidget* child = gtk_widget_get_first_child(page.notice)) {
        gtk_box_remove(GTK_BOX(page.notice), child);
    }
    page.notice_key = stopped;
    page.notice_words = nullptr;
    gtk_widget_set_visible(page.notice, stopped.has_value());
    if (!stopped) {
        return;
    }
    GtkWidget* notice = line_box("va-neutral", "media-playback-pause-symbolic", "dim-label");
    page.notice_words = words_label(shown.words);
    gtk_widget_set_hexpand(page.notice_words, TRUE);
    gtk_box_append(GTK_BOX(notice), page.notice_words);
    gtk_box_append(GTK_BOX(notice), row_button(state, text("Resume"), archive,
                                        [key = *stopped](GuiState& state, const Uuid& archive) {
                                            resume_in_details(state, archive, key);
                                        }));
    gtk_box_append(GTK_BOX(page.notice), notice);
}

} // namespace va::gui
