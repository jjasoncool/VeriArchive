#pragma once
// The GUI's state, its window (decision 0046: a sidebar of pages beside the
// page shown, the header's jobs button) and its pages: Archives (the list of
// archives beside the details of the one selected, where verifications start,
// and the unfinished operations above them), History (the runs of every
// archive) and Settings of HANDOFF §B17, Devices (the devices attached now,
// decision 0046) and Jobs (decision 0045); the result
// page of a run, which Archives and History open over themselves; and the
// dialogs of Register Archive and of a comparison (decision 0020).
#include "app/adding_chunk_hashes.h"
#include "app/archive_roots.h"
#include "app/refusal_in_window.h"
#include "app/unfinished_operations.h"
#include "app/session.h"
#include "core/engine/archive_status.h"
#include "core/interfaces.h"
#include "gui/archive_rows.h"
#include "gui/device_reads.h"
#include "gui/device_rows.h"
#include "gui/displayed_identity.h"
#include "gui/enclosure_flow.h"
#include "gui/gui_support.h"
#include "gui/history_rows.h"
#include "gui/job_words.h"
#include "gui/jobs.h"
#include "gui/register_choices.h"

#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace va::gui {

// Unfinished registrations, verifications and relocations, wherever they run
// (the command line, this window), read from the repository every few seconds
// while Archives or Jobs is shown (operations_poll.cpp). Each read runs on a thread of
// its own, so a slow one never holds up the window. Kept across windows, so a
// language change keeps the speed and time left.
struct OperationsPoll {
    std::thread thread;                         // the read in flight, or the last one
    bool busy = false;                          // a read is in flight (main thread only)
    std::chrono::steady_clock::time_point started{};  // when the last read began
    std::chrono::steady_clock::duration took{};       // how long the last read took
    guint timer = 0;                            // the GLib timeout; 0 while the list is hidden
    std::chrono::steady_clock::time_point ticked{};   // its last tick, or its start: a tick far later
                                                      // than this one tells that the window was held up
    GtkWidget* served = nullptr;                // the page (Archives or Jobs) the timer refreshes
    std::filesystem::path repository;           // the RepositoryRoot the watches belong to
    std::string archives_shown;                 // the mark of the archives the list shows (empty: none),
                                                // to tell when a read finds them changed
    std::vector<app::OperationWatch> watches;   // the operations of the latest read
    Report error;                               // why the latest read failed

    OperationsPoll() = default;
    OperationsPoll(const OperationsPoll&) = delete;
    OperationsPoll& operator=(const OperationsPoll&) = delete;
    // The program waits for a read still in flight before it ends.
    ~OperationsPoll()
    {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

// The devices and volumes attached now, with where each archive is attached
// (devices_poll.cpp), read while Archives or Devices is shown: when either
// comes on screen, when its Refresh is pressed, and every 30 seconds while one
// of them is shown; never once the window is gone. No platform tells of a
// device attached or removed, so reading again is the only way to see one.
// Each read runs on a thread of its own, which asks each device for its
// health once (on Linux a udisks call of up to a few seconds), so it never
// holds up the window; a slow read spaces out the next ones. Kept across
// windows, so a language change words the same read anew.
struct DevicesPoll {
    std::thread thread;                         // the read in flight, or the last one
    bool busy = false;                          // a read is in flight (main thread only)
    bool again = false;                         // Refresh was pressed, or the archives changed, meanwhile:
                                                // read once more once it is back
    std::chrono::steady_clock::time_point started{};  // when the last read began
    std::chrono::steady_clock::duration took{};       // how long the last read took
    std::chrono::steady_clock::time_point applied{};  // when the last read was shown (decision 0047)
    guint timer = 0;                            // the GLib timeout; 0 while neither page is shown
    GtkWidget* served = nullptr;                // the page (Archives or Devices) the timer serves
    // The RepositoryRoot whose archives the latest read matched (none: no
    // repository was open), the read, and why the latest read failed.
    std::optional<std::filesystem::path> repository;
    std::optional<AttachedRead> read;
    Report error;
    // A language chosen in Settings while a read was in flight, which
    // change_language makes once the read is back: on Linux the read asks
    // udisks, which reads the environment, and the change writes it
    // (use_gtk_language). The window is not held up meanwhile.
    std::optional<app::Language> language_after;

    DevicesPoll() = default;
    DevicesPoll(const DevicesPoll&) = delete;
    DevicesPoll& operator=(const DevicesPoll&) = delete;
    // The program waits for a read still in flight before it ends.
    ~DevicesPoll()
    {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

// The reads that the window's dialogs make off its thread (register_wizard.cpp):
// the attached volumes, which a slow or sleeping device may hold up, and the
// folder or volume chosen. Each runs on a thread of its own and hands its
// result to the main thread; a dialog closed meanwhile drops it. Kept across
// windows; the program waits for the reads still in flight before it ends.
struct OffThreadReads {
    struct Read {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::list<Read> reads;

    // Joins the reads that have finished; the others go on (main thread only).
    void prune()
    {
        for (auto read = reads.begin(); read != reads.end();) {
            if (read->finished->load()) {
                read->thread.join();
                read = reads.erase(read);
            } else {
                ++read;
            }
        }
    }

    OffThreadReads() = default;
    OffThreadReads(const OffThreadReads&) = delete;
    OffThreadReads& operator=(const OffThreadReads&) = delete;
    ~OffThreadReads()
    {
        for (auto& read : reads) {
            if (read.thread.joinable()) {
                read.thread.join();
            }
        }
    }
};

struct GuiState {
    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;           // an AdwApplicationWindow
    // The pages, each a child named as the sidebar names it (archives,
    // devices, jobs, history, settings): the one shown is the window's
    // content, whichever way it was chosen.
    GtkWidget* stack = nullptr;
    app::Text text;                        // interface language (decision 0017)
    HostInfo host;
    std::optional<app::Session> session;   // empty until a repository is open
    Report session_error;                  // why no repository is open
    // Reads a device's own identity behind its enclosure, with administrator
    // permission that Windows asks the operator for, on the operator's
    // request only (decision 0047; none on Linux). Kept across windows; it is
    // declared before every member that runs threads, so that it outlives
    // them (members go in the reverse order).
    std::unique_ptr<IDeviceIdentityHelper> device_identity;
    // The registrations, verifications, comparisons, relocations and exports
    // started in the window, each a job of its own (decisions 0045, 0046).
    Jobs jobs;
    // A step that runs besides the jobs, one at a time: the upgrade of the
    // repository's database as it opens (decision 0032), abandoning an
    // unfinished operation (decisions 0025, 0026, 0033), and retiring an
    // archive (decisions 0003, 0046).
    Job job;
    OperationsPoll operations;             // every unfinished operation, for Jobs and Archives
    DevicesPoll devices;                   // the devices attached now, for Devices and Archives
    // The roots of the registration jobs that wait or work, by job, which
    // Devices names on their volumes in place of Register this volume.
    std::map<std::uint64_t, std::filesystem::path> registering;
    // Decision 0047: the reads of devices' own identities behind their
    // enclosures that the operator asked for, kept while the system describes
    // each device as it did then and the devices are watched (device_reads.h),
    // which Devices shows and Register Archive records with no second question
    // of Windows (the enclosure question always reads anew); and the read in
    // flight, one at a time, which waits for Windows' question for
    // administrator permission, then for the device identity reader
    // (enclosure_dialog.cpp), and which closing the window tells to stop.
    KeptDeviceReads device_reads;
    struct DeviceReading {
        std::string device_ref;
        std::shared_ptr<CancelToken> cancel;
        std::uint64_t id = 0;
    };
    std::optional<DeviceReading> device_reading;
    std::uint64_t device_reads_started = 0;
    // The confirmation that each verification job of the window went on with
    // past a changed enclosure (given in the enclosure question, or carried
    // by the run that a resume goes on with), by the job, until it ended: its
    // archive's details say so meanwhile.
    std::map<std::uint64_t, CarrierConfirmation> confirmed_verifications;
    // What each archive's confirmed verification that ended recorded of its
    // new enclosure (only carrier_* are set), and when, until a read of the
    // attached devices begun since is shown: the read before matched the
    // archive's earlier carrier, and would ask about the enclosure once more
    // meanwhile.
    struct CarrierRecorded {
        VerifyOutcome carrier;
        std::chrono::steady_clock::time_point at;
    };
    std::map<Uuid, CarrierRecorded> carriers_recorded;
    OffThreadReads reads;                  // what Register Archive reads off the window's thread
    int exit_status = 0;                   // 1 when an unexpected error cut the smoke test, the start or the screenshots short
    // Set from VERIARCHIVE_GUI_SCREENSHOTS: take the documentation screenshots
    // into this folder, then quit (screenshots.cpp).
    std::optional<std::filesystem::path> screenshot_folder;
    // The media identity the views show, through displayed_identity: Sample
    // while the screenshot mode runs, as its pictures are published;
    // otherwise what was recorded.
    IdentityShown identity_shown = IdentityShown::Recorded;

    // What Archives shows, kept across windows: the archive selected, whose
    // details show beside the list; the job that each archive's details show
    // (decision 0045: the last verification or relocation started there, or
    // by Resume of its stopped one); and the run that the window interrupted
    // last (R-104), which Resume in its archive's details continues as the
    // window ran it (details_resume).
    std::optional<Uuid> archive_selected;
    std::map<Uuid, std::uint64_t> verification_jobs;
    std::optional<InterruptedRun> interrupted_run;

    // The run that each ended verification job of the window recorded, by
    // the job: View result on its rows in Jobs and in its details opens the
    // run's result page. Kept across windows; Clear ended jobs drops the
    // runs of the jobs it clears.
    struct JobRun {
        std::string run_id;
        Uuid archive_uuid;
    };
    std::map<std::uint64_t, JobRun> job_runs;

    // What History shows, kept across windows: the archive its filter names
    // (none: every archive), and what the last check of the chain found
    // (nothing until it is checked), worded as it is shown.
    std::optional<Uuid> history_filter;
    std::optional<std::expected<HistoryCheck, IoError>> history_check;

    // Decision 0048: the folder that Save report... of a result page last
    // saved a verification report into in this window, where its dialog
    // opens next (else the Documents folder); the reports being made and
    // written off the window's thread; and what the last one written ended
    // with, its file and why it was not saved (nothing where it was), which
    // the screenshot tour waits for. Kept across windows.
    std::optional<std::filesystem::path> report_folder;
    int reports_saving = 0;
    struct ReportSaved {
        std::filesystem::path file;
        Report failure;
    };
    std::optional<ReportSaved> report_saved;

    // The widgets below belong to the current window; a language change
    // replaces them. The Reports are what a view shows, kept so the new window
    // can word them in the new language.

    // The window around the pages (decision 0046).
    struct {
        GtkWidget* split = nullptr;        // AdwNavigationSplitView: the sidebar beside the page shown
        GtkWidget* sidebar = nullptr;      // GtkListBox, a row for each page
        GtkWidget* add = nullptr;          // Register Archive, the button above the sidebar
        GtkWidget* title = nullptr;        // AdwWindowTitle above the sidebar: VeriArchive, the repository's folder
        GtkWidget* content = nullptr;      // AdwNavigationPage of the page shown, titled as it
        GtkWidget* toasts = nullptr;       // AdwToastOverlay over the whole window
        GtkWidget* jobs_badge = nullptr;   // the Jobs row's count of jobs that run or wait
        GtkWidget* stopped_badge = nullptr;  // ... and of the unfinished operations that stopped (decision 0049)
        AdwBreakpoint* narrow = nullptr;   // the window's breakpoint, below which it shows one pane at a time
        AdwToast* toast = nullptr;         // the last toast while it is shown or waits (a weak pointer: nothing once gone)
        AdwToast* started = nullptr;       // the last toast that a job started, which the next toast replaces (weak too)
        bool asking_to_close = false;      // the question before the window closes is shown
    } shell;

    // A job's row, as the Jobs page, the header's popover and an archive's
    // details show it (jobs_page.cpp).
    struct JobRow {
        GtkWidget* title = nullptr;
        GtkWidget* meta = nullptr;         // job_meta: how far it is, whom it waits for, its result
        GtkWidget* tone = nullptr;         // once it ended: the icon of its result's tone, before `meta`
        GtkWidget* bar = nullptr;          // while it runs
        GtkWidget* alert = nullptr;
        GtkWidget* stop = nullptr;         // until it ended
        GtkWidget* result = nullptr;       // View result, once it ended having recorded a run (job_runs)
        GtkWidget* details = nullptr;
        JobGroup group = JobGroup::Waiting;  // the group the row is listed in
    };
    struct JobList {
        GtkWidget* box = nullptr;          // its groups, or that there are no jobs
        std::map<std::uint64_t, JobRow> rows;
    };

    // A row of the unfinished operations in Jobs as it is drawn: what
    // makes it up (its operation, its buttons, whether it has a bar and
    // details), and the widgets whose words and bar a later read changes. A
    // read that finds the same rows changes only those, so the keyboard
    // focus on a row's Resume or Abandon... stays where it is.
    struct OperationShown {
        std::string key;
        bool offers_resume = false;
        bool resume_asks_first = false;
        bool abandonable = false;
        bool has_bar = false;
        bool has_details = false;
        GtkWidget* icon = nullptr;
        GtkWidget* title = nullptr;
        GtkWidget* root = nullptr;
        GtkWidget* bar = nullptr;
        GtkWidget* progress = nullptr;
        GtkWidget* details = nullptr;
        GtkWidget* state = nullptr;
    };

    // Where an archive of the list of Archives is attached (archive_attached):
    // the first line of its row, which a read of the attached devices changes
    // in place, with the row and its words for a screen reader without it.
    struct AttachedLineShown {
        GtkWidget* row = nullptr;
        GtkWidget* line = nullptr;
        std::string said;
        std::optional<AttachedLine> shown;
    };

    // Archives (decision 0046): the page's head; the list of archives beside
    // the details of the one selected, or, without archives, a page that
    // leads to the first registration. A narrow window shows the list or the
    // details. The unfinished operations show in Jobs (decision 0049).
    struct {
        GtkWidget* navigation = nullptr;       // AdwNavigationView: the page, the result pages opened over it
        GtkWidget* root = nullptr;             // its first page (AdwNavigationPage), which holds `page`
        GtkWidget* page = nullptr;             // the page: the operations are read while it is shown
        GtkWidget* status = nullptr;           // how many archives, or why none can be listed
        GtkWidget* devices_problem = nullptr;  // that the attached devices cannot be read (Devices says why);
                                               // hidden while they can
        GtkWidget* refresh = nullptr;          // Refresh, which holds the focus while what had it is made anew
        GtkWidget* abandoning = nullptr;       // progress while a step of abandoning a registration
                                               // (decision 0025), a relocation (decision 0026) or a
                                               // verification that adds chunk hashes (decision 0033) runs,
                                               // or of retiring an archive (decision 0046)
        GtkWidget* views = nullptr;            // GtkStack: "archives" (the split view), or "empty"
        GtkWidget* empty = nullptr;            // AdwStatusPage: no archives yet, or no repository
        GtkWidget* empty_button = nullptr;     // what it leads to: Register Archive, or Settings
        GtkWidget* split = nullptr;            // AdwNavigationSplitView: the list beside the details
        GtkWidget* list = nullptr;             // GtkListBox, a row for each archive
        GtkWidget* detail_page = nullptr;      // AdwNavigationPage of the details, titled by the archive
        GtkWidget* detail = nullptr;           // the details of the archive selected, made again for each
        GtkWidget* back = nullptr;             // back to the list, shown in a narrow window
        GtkWidget* notice = nullptr;           // the stopped verification of the archive shown, from the
                                               // latest read of the unfinished operations
        std::optional<std::string> notice_key; // the operation `notice` shows, once it shows one
        GtkWidget* notice_words = nullptr;     // its words, which a later read of it changes in place
        std::map<Uuid, AttachedLineShown> attached_lines;  // where each archive listed is attached
        // In the details: the notice of an archive not attached as recorded;
        // the group Device, whose one row holds the fields of its carrier as
        // recorded (`device_fields`) and where it is attached, as the latest
        // read found it (attached_shown); and whether FULL verify is not
        // offered because the archive's job waits or works.
        GtkWidget* attached_notice = nullptr;
        GtkWidget* attached_words = nullptr;
        GtkWidget* device_group = nullptr;
        GtkWidget* device_row = nullptr;
        std::vector<Field> device_fields;
        std::optional<ArchiveAttached> attached_shown;
        bool verify_busy = false;
        GtkWidget* verify = nullptr;           // FULL verify (AdwSplitButton)
        GtkWidget* view_result = nullptr;      // View result of the last FULL verification, where there is one
        GtkWidget* verify_menu = nullptr;      // the popover of the menu of FULL verify
        GtkWidget* adding = nullptr;           // its "FULL verify, and add chunk hashes", where it lists it
        GtkWidget* adding_note = nullptr;      // the line under it: what it does, or why it is not offered
        // Why chunk hashes cannot be added to the archive shown, as
        // why_chunk_hashes_cannot_be_added said it when the details were made.
        std::expected<std::optional<std::string>, IoError> adding_refused;
        JobList card;                          // the archive's job, as a card in its details
        std::vector<ArchiveStatus> statuses;   // the archives the list shows, in its order
        bool filling = false;                  // the list is being filled: a row selected is not the operator's
    } archives_page;

    // Register Archive (decision 0046): an AdwDialog of three pages in an
    // AdwNavigationView: Choose the source (the attached volumes, read off
    // the window's thread, and a folder chosen or typed), Options (those that
    // apply to the source, as the read of it found them) and Confirm (what
    // the registration will do), whose Start registration starts its job
    // (decision 0045). Its widgets while it is open; nothing otherwise.
    struct RegisterWizard {
        GtkWidget* dialog = nullptr;            // AdwDialog
        GtkWidget* navigation = nullptr;        // AdwNavigationView of its pages
        GtkWidget* source_page = nullptr;       // AdwNavigationPage: Choose the source
        GtkWidget* options_page = nullptr;      // ... Options, once the source was read
        GtkWidget* confirm_page = nullptr;      // ... Confirm
        // Choose the source.
        GtkWidget* volumes = nullptr;           // AdwPreferencesGroup of the attached volumes
        std::vector<GtkWidget*> volume_rows;    // its rows, as it shows them now
        std::vector<GtkWidget*> radios;         // the radio of each volume that can be chosen whole
        std::vector<std::string> radio_roots;   // the root that each of them chooses
        GtkWidget* folder = nullptr;            // AdwEntryRow: the folder or volume chosen, or typed
        GtkWidget* problem = nullptr;           // why the source cannot be registered, hidden while none
        GtkWidget* problem_words = nullptr;
        GtkWidget* source_next = nullptr;       // Next
        GtkWidget* reading = nullptr;           // GtkSpinner beside it while the source is read
        // Options.
        GtkWidget* label = nullptr;             // AdwEntryRow
        GtkWidget* label_group = nullptr;       // its group, with the line about the next label
        GtkWidget* new_revision = nullptr;      // AdwSwitchRow
        GtkWidget* another_archive = nullptr;   // AdwSwitchRow: --force-new-archive
        GtkWidget* resume = nullptr;            // AdwActionRow, no switch: the resume is the only way on there
        GtkWidget* media_copy = nullptr;        // AdwSwitchRow
        GtkWidget* copy_of = nullptr;           // AdwComboRow: not a copy, then the archives (copy_of_uuids)
        GtkWidget* blocked = nullptr;           // why Next is not offered yet, hidden while none
        GtkWidget* blocked_words = nullptr;
        GtkWidget* options_next = nullptr;      // Next
        std::vector<Uuid> copy_of_uuids;        // item i + 1 of copy_of
        // Confirm.
        GtkWidget* start = nullptr;             // Start registration
        // What the reads found: the archives' names, as Archives names them;
        // the source chosen, which options apply to it, and what was chosen.
        std::vector<ArchiveName> names;
        std::optional<ChosenSource> source;
        SourceOptions options;
        OptionsChosen chosen;
        // Opened by Create a new revision... of this archive: Create a new
        // revision on, once a source where it is registered was read; until
        // then, any other source is refused, saying so.
        std::optional<Uuid> preset_archive;
        // ... and the root its latest revision recorded, read once the
        // attached volumes were (where they hold the archive, if they do).
        std::optional<std::string> preset_root;
        // Opened by Register as another archive... of the enclosure question
        // (decision 0047): once its root is read, a separate archive, a copy
        // of this one.
        std::optional<Uuid> preset_copy;
        // Decision 0047: Record the device's own serial, where the source's
        // enclosure hides it (Options: an AdwSwitchRow, or the serial kept
        // from before in an AdwActionRow); on Confirm, why the read that
        // Start registration made first gave nothing, and the spinner beside
        // it while that read is in flight.
        std::optional<DeviceSerialOffer> serial_offer;
        GtkWidget* device_serial = nullptr;
        GtkWidget* device_serial_kept = nullptr;
        GtkWidget* serial_notice = nullptr;
        GtkWidget* serial_notice_words = nullptr;
        GtkWidget* summary = nullptr;           // Confirm's boxed list of what it will do
        GtkWidget* starting = nullptr;
        std::optional<ReadProblem> serial_problem;
        bool reading_serial = false;
        std::shared_ptr<CancelToken> serial_cancel;  // stops that read's wait once the dialog closes
        bool reading_source = false;            // a read of the source is in flight
        bool filling = false;                   // a change of the code's, not the operator's
        std::uint64_t opened = 0;               // which opening of the dialog this is, whose reads it takes
    } register_wizard;
    std::uint64_t wizards_opened = 0;           // the openings of Register Archive so far

    // The comparison dialog (decisions 0020, 0046): the reference archive
    // (A) and the copy to check (B), the copies of A first in B's list, and
    // why the two cannot be compared as chosen; Compare starts its job. Its
    // widgets while it is open; nothing otherwise.
    struct CompareDialog {
        GtkWidget* dialog = nullptr;            // AdwDialog
        GtkWidget* reference = nullptr;         // AdwComboRow: A
        GtkWidget* copy = nullptr;              // AdwComboRow: B
        GtkWidget* refusal = nullptr;           // why A and B cannot be compared, hidden while they can
        GtkWidget* refusal_words = nullptr;
        GtkWidget* start = nullptr;             // Compare
        std::vector<Uuid> references;           // item i of A
        std::vector<Uuid> copies;               // item i of B
        bool filling = false;                   // a change of the code's, not the operator's
    } compare_dialog;

    // The relocation dialog (decisions 0004, 0046): what relocation does,
    // the attached volumes (read off the window's thread, a fixed sample list
    // in the screenshot mode) and the new root, typed, pasted or chosen, and
    // what the relocation will do; Start relocation checks the new root off
    // the window's thread, saying in the dialog why it is refused, and once
    // it is taken starts its job (decision 0045). Where the archive's
    // relocation stopped, the dialog resumes it (Resume relocation), its root
    // as its record names it, or another where the new carrier is attached at
    // another drive letter or mount point now. Its widgets while it is open;
    // nothing otherwise.
    struct RelocateDialog {
        GtkWidget* dialog = nullptr;            // AdwDialog
        Uuid archive;                           // the archive it relocates
        std::optional<app::UnfinishedOperation> resuming;  // the stopped relocation it resumes, as listed
        GtkWidget* volumes = nullptr;           // AdwPreferencesGroup of the attached volumes
        std::vector<GtkWidget*> volume_rows;    // its rows, as it shows them now
        std::vector<GtkWidget*> radios;         // the radio of each volume that can be chosen whole
        GtkWidget* folder = nullptr;            // AdwEntryRow: the new root
        GtkWidget* problem = nullptr;           // AdwActionRow under it: why the new root is refused, hidden without
        GtkWidget* summary = nullptr;           // AdwPreferencesGroup: what it will do
        GtkWidget* summary_row = nullptr;       // its one row of fields
        GtkWidget* start = nullptr;             // Start relocation, or Resume relocation
        GtkWidget* checking = nullptr;          // GtkSpinner beside it while the new root is checked
        bool checking_root = false;             // the new root is being checked
        bool filling = false;                   // a change of the code's, not the operator's
        std::uint64_t opened = 0;               // which opening of the dialog this is, whose reads it takes
    } relocate_dialog;
    std::uint64_t relocations_opened = 0;       // the openings of the relocation dialog so far

    // The enclosure question (decision 0047): what FULL verify of an archive
    // whose device is seen behind another enclosure asks first, an AdwDialog
    // whose content is made again for each step (enclosure_flow.h). Its
    // widgets and what it asks about while it is open; nothing otherwise.
    struct EnclosureDialog {
        GtkWidget* dialog = nullptr;            // AdwDialog
        GtkWidget* content = nullptr;           // the step shown
        GtkWidget* answers = nullptr;           // its answers' buttons
        Uuid archive;
        bool strict = false;                    // FULL verify with strict metadata
        EnclosureCase found;
        app::EnclosureFacts facts;
        EnclosureFlow flow;
        std::shared_ptr<CancelToken> cancel;    // stops the wait of its read, once Windows' question is answered
        std::uint64_t opened = 0;               // which opening of the dialog this is, whose read it takes
    } enclosure_dialog;
    std::uint64_t enclosures_opened = 0;

    // The Jobs page and the header's jobs button (decisions 0045, 0046): a
    // row for each job in the group it is in, the button's popover with the
    // jobs that run and wait and the last ended ones, and the dialog of
    // details of each job whose Details was opened.
    struct JobDetails {
        GtkWidget* dialog = nullptr;       // AdwDialog
        GtkWidget* state = nullptr;
        GtkWidget* bar = nullptr;
        GtkWidget* alert = nullptr;
        GtkWidget* report = nullptr;
        GtkWidget* stop = nullptr;
        GtkWidget* result = nullptr;       // View result, as on its row
    };
    struct {
        JobList page;                      // the Jobs page
        GtkWidget* clear = nullptr;        // "Clear ended jobs"
        // The unfinished operations (decision 0022), wherever they were
        // started, below the jobs (decision 0049): those a job of this
        // window works on are its rows' already. Hidden when there are none.
        GtkWidget* operations = nullptr;       // the group, AdwPreferencesGroup
        GtkWidget* operations_note = nullptr;  // what it shows, or why it cannot
        GtkWidget* operations_list = nullptr;  // GtkListBox, a row for each operation
        std::vector<OperationShown> operation_rows;  // its rows, in its order
        GtkWidget* abandoning = nullptr;       // progress while a step of abandoning one of them runs
                                               // (decisions 0025, 0026, 0033)
        GtkWidget* stopped = nullptr;      // on the jobs button: how many of them stopped, hidden at 0
        GtkWidget* stopped_count = nullptr;
        GtkWidget* button = nullptr;       // the header's jobs button (GtkMenuButton)
        GtkWidget* ring = nullptr;         // its ring: how far the jobs that run are (running_fraction)
        GtkWidget* count = nullptr;        // its count of jobs that run or wait, hidden at 0
        GtkWidget* word = nullptr;         // its word "Jobs", hidden in a narrow window
        GtkWidget* popover = nullptr;
        GtkWidget* popover_box = nullptr;  // what the popover holds, narrower in a narrow window
        JobList popover_list;              // the popover's jobs
        std::map<std::uint64_t, JobDetails> details;
    } jobs_page;

    // History (decision 0046): the runs of every archive, or of the archive
    // the filter names, newest first, a page of them at a time; each row
    // opens the run's result page over it. Above them the result of the
    // last check of the chain.
    struct {
        GtkWidget* navigation = nullptr;   // AdwNavigationView: the page, the result pages opened over it
        GtkWidget* root = nullptr;         // its first page (AdwNavigationPage)
        GtkWidget* filter = nullptr;       // AdwComboRow: every archive, then each archive
        GtkWidget* check = nullptr;        // "Check history chain"
        GtkWidget* chain = nullptr;        // the check's result, hidden until checked
        GtkWidget* chain_result = nullptr;
        GtkWidget* chain_limits = nullptr;
        GtkWidget* list = nullptr;         // GtkListBox, a row for each run listed
        std::vector<Uuid> filter_uuids;    // item i + 1 of the filter
        std::vector<ArchiveName> names;    // the archives, as the last read named them
        std::vector<VerifyRunRecord> runs; // every run, as the last read found them, oldest first
        Report unread;                     // why the last read failed
        std::size_t listed = 0;            // the rows listed of the filter's runs
        bool filling = false;              // the filter is being filled: a choice is not the operator's
    } history_page;

    // Devices (decision 0046): the devices attached now, as the latest read
    // found them, each a card with its volumes and the archives on them.
    struct {
        GtkWidget* page = nullptr;         // the page: the devices are read while it is shown
        GtkWidget* status = nullptr;       // how many devices are attached, or that none is
        GtkWidget* reading = nullptr;      // GtkSpinner while a read is in flight
        GtkWidget* refresh = nullptr;      // Refresh, which holds the focus while what had it is made anew
        GtkWidget* problem = nullptr;      // why the latest read failed, or why the archives cannot be told;
        GtkWidget* problem_words = nullptr;  // hidden while there is nothing to say
        GtkWidget* cards = nullptr;        // GtkBox: a card for each device, or what is read
        // What reading a device's own serial behind its enclosure is for
        // (decision 0047), shown where this computer can read one.
        GtkWidget* enclosures = nullptr;
        std::optional<std::vector<DeviceCard>> shown;  // what the cards show, once a read was drawn
    } devices_page;

    // Settings, an AdwPreferencesPage (decision 0046): its groups, and the
    // rows that refresh_settings put into them.
    struct {
        GtkWidget* repository = nullptr;   // RepositoryRoot and how it was chosen, or why none is open
        GtkWidget* language = nullptr;     // AdwComboRow: English, 繁體中文
        GtkWidget* policy = nullptr;       // overdue limits, strict metadata, trusted keys
        GtkWidget* about = nullptr;        // the program's version, what it detects, the notice
        std::vector<std::pair<GtkWidget*, GtkWidget*>> rows;  // (group, row) that refresh_settings added
        Report notice;                     // e.g. the language could not be saved
    } settings_page;
};

// Archives and History are each an AdwNavigationView (their `navigation`)
// whose first page is the page itself, over which a result page opens.
GtkWidget* build_archives_page(GuiState& state);
GtkWidget* build_devices_page(GuiState& state);
GtkWidget* build_history_page(GuiState& state);
GtkWidget* build_settings_page(GuiState& state);
GtkWidget* build_jobs_page(GuiState& state);

// The icon of the Jobs page on its sidebar row, beside its name, and of an
// unfinished operation that works. Not the circling arrows of the approved
// design of decision 0046: those are Refresh in the head of Archives, beside
// them on one screen. The header's jobs button has a ring of its own
// instead, beside its word.
inline constexpr const char* jobs_icon = "system-run-symbolic";

// The width of what the jobs popover holds, and its width in a narrow window
// (whose least width is 360), where it would reach past the window.
inline constexpr int jobs_popover_width = 420;
inline constexpr int jobs_popover_narrow_width = 300;

// The header's jobs button (decision 0046): a ring of how far the jobs that
// run are, the count of the jobs that run or wait and the word Jobs, and its
// popover, which lists them and the last 3 ended and leads to the Jobs page.
GtkWidget* build_jobs_button(GuiState& state);

// Creates the window, in state.text's language (window.cpp): the sidebar of
// pages beside the page shown, the header's jobs button, and the question
// before the window closes while jobs run. It shows Archives.
void build_window(GuiState& state);

// Shows the page named `page` (a child of state.stack), itself rather than a
// result page opened over it; the sidebar selects its row, the header names
// it, and a narrow window shows it in place of the sidebar.
void show_page(GuiState& state, const char* page);

// The window's narrow layout (window.cpp): sets `property` of `object` to
// `value` while the window is narrower than its breakpoint, as the pages ask
// it while they are built (Archives shows its list or the details of an
// archive, not both). Once build_window has made the breakpoint.
void set_while_narrow(GuiState& state, GtkWidget* object, const char* property, bool value);

// The window's own part of what a repository changes: the repository's
// folder under the sidebar's title.
void refresh_shell(GuiState& state);

// A short confirmation at the bottom of the window (AdwToast), with a button
// that runs `action` when there is one; it goes by itself. It shows at once:
// a toast shown before waits behind it, and shows again after it, so the
// result of a verification (FAILED, say) is never lost to a later toast.
// Only a toast that says a job started (ToastKind::Started) gives way: the
// next toast takes its place, as the end of the job says more.
struct ToastAction {
    std::string label;
    std::function<void(GuiState&)> run;
};
enum class ToastKind { Lasting, Started };
void show_toast(GuiState& state, const std::string& text, std::optional<ToastAction> action = std::nullopt,
    ToastKind kind = ToastKind::Lasting);

// Takes the last toast, and that of a job started, away at once (the
// screenshot tour, before its next picture).
void dismiss_toast(GuiState& state);

// What the window does once a job changed (Jobs::on_change; 0: jobs were
// added or removed): the Jobs page, the header's jobs button and its
// popover, the job's details, and the view that started it show it as it is
// now, and the language drop-down is locked while a job waits or works.
void job_changed(GuiState& state, std::uint64_t job);

// Shows the details of `job` in a dialog of their own, or nothing for a job
// removed.
void open_job_details(GuiState& state, std::uint64_t job);

// Closes every dialog of a job's details (before a language change replaces
// the main window); returns the jobs whose details were open, so the new
// window can show them again.
std::vector<std::uint64_t> close_job_details(GuiState& state);

// A card of `job` in `list` (an archive's details): its row as the Jobs page
// shows it, with its title, how far it is, its device alert, its bar, Stop and
// Details; job_changed keeps it up to date while `list` holds it.
GtkWidget* make_job_card(GuiState& state, GuiState::JobList& list, const JobShown& job);

// The devices that a job of `root`, or of the archive `archive_uuid`, reads
// (device_keys, recorded_device_keys), as the device queue compares them;
// asked on the job's thread. None where they cannot be told: the job then
// waits for nothing, and its operation says what is wrong.
std::vector<std::string> devices_of_root(const std::filesystem::path& root);
std::vector<std::string> devices_of_archive(const RepositoryLayout& layout, const Uuid& archive_uuid);

// Whether a job of the window waits or works on the operation with `root`
// or `archive` (either may be empty), which a step that abandons or resumes
// it must not take over (decision 0045); if so, says so in a message.
bool refused_while_a_job_reads(GuiState& state, const std::string& root, const Uuid& archive);

// Reload data shown by the views (after the repository changes).
// refresh_archives reads the archives again and shows them, the details of
// the one selected, and the unfinished operations of the latest read; the
// reads of those run while Archives is shown (watch_operations). An open
// comparison dialog lists the archives read, keeping A and B where it can.
void refresh_archives(GuiState& state);
void refresh_settings(GuiState& state);

// ---- Archives (archives_page.cpp, archive_detail.cpp) -----------------------

// Selects `archive` in the list of Archives and shows its details; in a narrow
// window with `show_details`, in place of the list. Nothing for an archive
// the list does not show.
void select_archive(GuiState& state, const Uuid& archive, bool show_details = false);

// The details of the archive selected (state.archive_selected), made again
// from what the list read; the first archive's where none is selected.
void show_details(GuiState& state);

// The part of the details that the reads of the unfinished operations change
// (details_operations): the notice of a stopped verification of the archive
// shown, which Resume continues, and whether "FULL verify, and add chunk
// hashes" is offered while such a verification is unfinished.
void show_details_operations(GuiState& state);

// Draws the unfinished operations of the latest read in Jobs, in the
// interface language, hidden when nothing is unfinished; counts those that
// stopped on the jobs button and the Jobs row (decision 0049); and shows
// the details' and result pages' parts that they change.
void show_operations(GuiState& state);


// The unfinished operations that have stopped, of the latest read, which
// Resume or Abandon... in Jobs can finish.
std::size_t stopped_operations(const GuiState& state);

// Says `report` in the head of Archives, where an unexpected error of its
// handlers shows.
void show_archives_status(GuiState& state, const Report& report);

// The part of Archives that the reads of the attached devices change
// (archive_attached): the first line of each row, where its archive is
// attached now, or that it is not; and in the details the notice of an
// archive not attached as recorded, the tooltip of FULL verify, and where the
// archive is attached in its group Device (show_details_attached). Each
// changes only where the read changed it, so the keyboard focus stays.
void show_attached(GuiState& state);
void show_details_attached(GuiState& state);

// ---- The attached devices (devices_poll.cpp) and Devices (devices_page.cpp) --

// Why the attached devices are read now: Archives or Devices came on screen;
// the operator pressed Refresh there, or the archives changed (a read in
// flight is followed by another once it is back); the timer ticked (a slow
// read spaces it out, read_due).
enum class DevicesReadWhy { Shown, Refresh, Due };

// Reads the attached devices and volumes on a thread, unless a read is in
// flight or the window is gone, and the candidates of the repository's
// archives to tell which lies where; the result comes back to the main
// thread, which shows it in Archives and Devices. In the screenshot mode
// Devices gets the fixed sample list (sample_volumes), and both where the
// sample archives are on the volumes of the sample archives' folders, the only
// volumes of this computer then read, shown through the sample identity.
void read_devices(GuiState& state, DevicesReadWhy why);

// Archives or Devices came on screen (`page`): reads now, then every 30
// seconds. It left the screen (another page, or its window closed): no more
// reads for it.
void watch_devices(GuiState& state, GtkWidget* page);
void stop_watching_devices(GuiState& state, GtkWidget* page);

// The latest read, where it matched the archives of the repository open now
// (or none open); null before the first read, after one that failed, and once
// another repository was opened.
const AttachedRead* attached_now(const GuiState& state);

// The roots of the archives on the attached volumes, as the latest read of
// them found them (MATCH, MATCH_WEAK, MIGRATED, AMBIGUOUS alike: each is an
// archive's folder there), named as Archives names them: where neither an
// export (export_refusal) nor a verification report (app::report_refusal)
// is written. Nothing before the first read.
std::vector<app::AttachedArchiveRoot> attached_roots(const GuiState& state);

// Draws Devices from the latest read: a card for each device, the problem of
// the read, or that it is being read.
void refresh_devices(GuiState& state);

// Before a part of Archives (`part`) is made anew: where it holds the
// keyboard focus, the focus goes to the page's Refresh, so that it never goes
// with the widgets taken away; returns whether it did, for the part to take
// the focus back once it is made.
bool keep_focus_from(GuiState& state, GtkWidget* part);

// A pill (archive_rows.h) and a status dot, in the colour of their tone
// (tone_class, the window's style).
GtkWidget* make_pill(const Pill& pill);
GtkWidget* make_dot(Tone tone);

// What the details and the result pages are made of (archive_detail.cpp):
// words that wrap anywhere in a long name, in `style`, selectable where they
// may be copied; the icon of a banner's tone; a line of a device alert or a
// notice, in the colour of its kind (va-error, va-neutral), its icon first,
// the rest appended; and a row of a boxed list with each field's name beside
// its value, which can be selected and copied.
GtkWidget* make_words(const std::string& text, const char* style = nullptr, bool selectable = false);
const char* tone_icon(Tone tone);
GtkWidget* make_line_box(const char* kind, const char* icon_name, const char* icon_style);
GtkWidget* make_fields_row(const std::vector<Field>& fields);

// ---- The unfinished operations (operations_poll.cpp) ------------------------

// The watches belong to one repository; another one opened in Settings
// starts afresh.
void use_session_repository(GuiState& state);

// Reads the unfinished operations on a thread, unless a read is in flight;
// the result comes back to the main thread, which shows it. `when_due`:
// called by the timer, which a slow read spaces out.
void start_poll(GuiState& state, bool when_due = false);

// Shows why the operations could not be read, keeping the rows of the last read.
void show_poll_error(GuiState& state, Report report);

// Archives came on screen (`page`): reads now, then every few seconds.
// It left the screen (another page, or its window closed): no more reads.
void watch_operations(GuiState& state, GtkWidget* page);
void stop_watching(GuiState& state, GtkWidget* page);

// ---- Resume and Abandon... of the unfinished operations (unfinished_actions.cpp)

// The operator chose Resume on the stopped verification, registration or
// relocation `key` of the latest read (decision 0022, point 7; decision 0046):
// it continues as its command-line resume does. ask_to_resume asks first, for
// a registration that
// an older version left and that may have stopped
// (OperationRow::resume_asks_first), and resumes only once the operator chose
// Resume. ask_to_abandon reads what abandoning the registration, relocation
// or verification that adds chunk hashes `key` removes, then asks (decisions
// 0025, 0026, 0033). Each says in a message why it cannot: no repository, a
// step of abandoning that runs, an operation no longer listed, or one that a
// job of the window works on.
void resume_from_list(GuiState& state, const std::string& key);
void ask_to_resume(GuiState& state, const std::string& key);
void ask_to_abandon(GuiState& state, const std::string& key);

// Whether the window can resume an operation now, as a job: not without a
// repository, nor while a step of abandoning runs, which may be abandoning
// it; otherwise says why not in a message.
bool can_resume(GuiState& state);

// Says `report` in a message of its own (AdwAlertDialog).
void show_message(GuiState& state, const Report& report);

// ---- Result pages (result_page.cpp) -----------------------------------------

// The pages over which a result page opens: Archives (from the details' View
// result and Recent verifications, and from the toast of a verification) and
// History (from its rows).
enum class ResultsOver { Archives, History };

// Opens the result page of the run `run_id` (decision 0046) over the page
// `over`, which it shows: an AdwNavigationPage pushed on its navigation view,
// whose back button returns to it and names it. Read from the repository's
// records of the run; a page that says so where they cannot be read.
void open_result(GuiState& state, ResultsOver over, const std::string& run_id);

// The result page of the run `run_id` of `archive` over Archives, with the
// details of that archive behind it, also in a narrow window: View result of
// a verification's toast, and of an ended verification's row in Jobs and its
// details.
void open_result_of(GuiState& state, const Uuid& archive, const std::string& run_id);

// The runs whose result pages are open over `over`, first opened first; and
// the same opened again over a new window's page (a language change), at once.
std::vector<std::string> results_open(const GuiState& state, ResultsOver over);
void open_results_again(GuiState& state, ResultsOver over, const std::vector<std::string>& runs);

// The part of the result pages that the reads of the unfinished operations
// and the jobs change: Resume on the page of an INCOMPLETE run, shown while
// the latest read lists its stopped verification with Resume (stopped_run)
// and its archive has no job of the window that waits or works.
void show_results_operations(GuiState& state);

// Save report... of the result page of the run `run_id` (decision 0048):
// asks where to save its verification report in a save dialog that
// proposes `proposed` (app::report_file_name) in the folder the last report
// went to in this window, or the Documents folder, and that asks before it
// replaces a file; then saves it there (save_report_to).
void save_report(GuiState& state, const std::string& run_id, const std::string& proposed);

// Makes the verification report of the run `run_id` from the repository's
// records as they are now, as the result page words them (its archive named
// as Archives lists it, the identity the views show: the sample ones in the
// screenshot mode), and writes it into `file`, off the window's thread
// (read_off_thread; short, so neither its progress nor a way to stop it is
// shown); then says in a toast that it was saved, with Open, which opens it
// as the system opens such a file, or why not. Refused in a message where
// the folder of `file` lies in the root of an archive attached now, as the
// latest read of the attached devices found them (attached_roots), and on the
// thread as a read of that folder's volume finds them, also through another
// name of the folder (app::roots_holding): the file would be an entry that
// the next FULL verification of that archive reports as added
// (app::report_refusal). state.report_saved says how it ended.
void save_report_to(GuiState& state, const std::string& run_id, const std::filesystem::path& file);

// The smoke test's check of the verification report (decision 0048), which
// no run of the smoke test's empty repository has: makes the report of a
// fixed run of sample data, made up for the check, as Save report... makes
// it, in the window's language, and checks that it is a whole page of that
// language that escapes what it shows, and that an INCOMPLETE run has none.
// Where VERIARCHIVE_GUI_SMOKE_CHART names a folder, writes it into it as
// sample-report.html, as Save report... writes a report. Throws where
// anything is wrong.
void check_sample_report(GuiState& state);

// The smoke test's check of the read speed chart (decision 0044), which no
// run of the smoke test's empty repository has: draws a fixed chart of sample
// data, made up for the check, with an earlier read and a markedly slower
// stretch, as a result page draws it, in the light and in the dark style,
// into images of its own, never on a run's page. Throws where it drew almost
// nothing; GTK says on the error output what else went wrong, which fails the
// smoke test. Where VERIARCHIVE_GUI_SMOKE_CHART names a folder, writes each
// image into it (sample-chart-light.png, sample-chart-dark.png).
void check_sample_chart(GuiState& state);

// ---- History (history_page.cpp) ---------------------------------------------

// Reads every run again, with the archives' names, and lists those of the
// archive the filter names (state.history_filter), or every archive's. A
// check of the chain that found it intact goes once a run was recorded since
// (chain_check_holds).
void refresh_history(GuiState& state);

// Says in History that its runs could not be read (`report`: an unexpected
// error of a read), in place of the runs of the last read.
void show_history_error(GuiState& state, Report report);

// Resume on a stopped verification or registration of the unfinished
// operations (decision 0022, point 7): continues it exactly as its
// command-line resume does, with the request that app::verification_resume
// and app::registration_resume make, as they make it: no choice of the views
// takes part. It runs as a job (decision 0045): a verification shown in its
// archive's details in Archives, which it brings into view; a registration in
// Jobs, a toast saying that it started and, once it ended, its result. The
// caller has made sure that a repository is open and that no job of the
// window reads the operation (refused_while_a_job_reads). `confirmed`: the
// operator chose Resume in the question that a resume of a registration an
// older version left asks first (ask_before_resuming), as `register <root>
// --resume --confirm`; a resume of such a registration that is not confirmed
// asks it once its job ended.
void resume_verification(GuiState& state, const app::UnfinishedOperation& verification);
void resume_registration(GuiState& state, const app::UnfinishedOperation& registration, bool confirmed = false);

// Asks, with Cancel as the default, before a registration that an older
// version of VeriArchive left is resumed, as that version may still run it
// (decision 0022, point 7; app::older_version_resume_warning): Resume under
// Unfinished operations on such a registration asks it, and so does the job
// of a resume of one, which Register Archive's Resume the interrupted
// registration or a stopped row's Resume starts, when the resume says it
// waits for the operator's word (RegisterFailure::older_version). `resume`
// runs only once the operator chose Resume; Cancel changes nothing.
void ask_before_resuming(GuiState& state, const std::string& label, const std::string& root,
    std::function<void(GuiState&)> resume);

// ---- Verifications (verification.cpp) ---------------------------------------

// How FULL verify of an archive's details reads it: in full; in full adding
// chunk hashes (decision 0033); in full with strict metadata (R-51).
enum class VerifyChoice { Full, AddingChunkHashes, StrictMetadata };

// FULL verify of `archive`, found by its uuid on the attached volumes, as a
// job titled by its label (decision 0045), shown in its details; strict
// metadata where `choice` asks for it or the policy sets it.
void verify_archive(GuiState& state, const Uuid& archive, VerifyChoice choice);

// "Verify a folder or volume...": asks, in a dialog over the window, for the
// folder or volume to verify, typed or pasted (a root that a refusal named,
// a volume's GUID path) or chosen with Choose... (GtkFileDialog), and
// whether to fail metadata differences; then verifies it by its root as
// verify_root does.
void choose_and_verify_root(GuiState& state, const Uuid& shown_in);

// A FULL verification of the archive that `root` holds, told by the root's
// identity, as a job titled by the root, shown in the details of `shown_in`
// (the archive whose details started it); for an archive that more than one
// attached volume holds, which a verification by uuid cannot tell apart.
// Strict metadata where `strict` asks for it or the policy sets it.
void verify_root(GuiState& state, const Uuid& shown_in, const std::filesystem::path& root, bool strict = false);

// Resume in the details of `archive` on its stopped verification `key` (as
// the latest read lists it), as details_resume makes its request: the run
// that this window interrupted last as the window ran it, where it is that
// one; otherwise as Resume on its row under Unfinished operations; and on its
// stopped relocation, as Resume on its row (resume_relocation).
void resume_in_details(GuiState& state, const Uuid& archive, const std::string& key);

// Whether `archive` (whose recorded root is `root`, which may be empty)
// already has a job of this window that waits or works on it or its root,
// which a second verification would only follow with another whole read of
// its device; if so, says so in a message (archive_has_a_job). Every
// verification that Archives starts asks it first.
bool refused_while_the_archive_has_a_job(GuiState& state, const Uuid& archive, const std::string& root);

// A FULL verification of `archive` by its uuid that goes on past the changed
// enclosure with `confirmation` (decision 0047), as the enclosure question
// gives it, strict metadata where `strict` asks for it or the policy sets it;
// a job shown in the archive's details, which say meanwhile that only a pass
// records the new enclosure, and whose end toast says what it recorded.
// False where it did not start (no repository open, a job of the archive
// began meanwhile), which a message says.
bool verify_confirmed(GuiState& state, const Uuid& archive, bool strict, CarrierConfirmation confirmation);

// ---- A device's own identity behind its enclosure (enclosure_dialog.cpp, decision 0047)

// What a read of a device's own identity gave: the read, or why none; and,
// read with it from the repository, the archives whose confirmed device it
// is (confirmed_device). `failure`: an exception of the read.
struct OwnIdentityRead {
    std::expected<DeviceIdentityRead, IoError> read =
        std::unexpected(IoError{IoErrorKind::Cancelled, 0, "not read"});
    std::vector<Uuid> confirmed_of;
    Report failure;
};

// Reads `device`'s own identity with the device identity reader, off the
// window's thread: Windows asks the operator for administrator permission
// first, which the window waits for without being held up. One read at a
// time: says so in a message, and reads nothing, while another waits or
// works. A read that gave the identity is kept (state.device_reads) and
// shown in Devices; `done` then gets it on the main thread, unless the
// window is gone. `cancel`, if given, stops the wait once Windows' question
// was answered (the dialog that asked for the read closed).
bool read_own_identity(GuiState& state, const PhysicalDeviceInfo& device,
    std::function<void(GuiState&, OwnIdentityRead&)> done, std::shared_ptr<CancelToken> cancel = nullptr);

// FULL verify of an archive whose device is seen behind another enclosure
// (`found`, enclosure_case): the enclosure question, an AdwDialog over the
// window, which leads to a FULL verification with a carrier confirmation
// (verify_confirmed), to Register Archive for the volume as another archive,
// or to nothing. It always opens on its first step, and its read always reads
// the device anew: a read kept from before cannot tell that the device behind
// the enclosure is still the one read (device_reads.h). `strict`: FULL verify
// with strict metadata.
void ask_about_enclosure(GuiState& state, const Uuid& archive, bool strict, const EnclosureCase& found);

// Read the device's own serial... of Devices: reads it (read_own_identity),
// then Devices shows it; a toast says why where it gave nothing.
void read_device_serial(GuiState& state, const std::string& device_ref);

// The smoke test's check of the enclosure question, which no archive of the
// smoke test's empty repository asks: makes it of sample data, made up for
// the check (SAMPLE-* enclosures and serials), without and with a confirmed
// device, and shows each of its steps in turn, never presented, nothing read
// or verified. Throws where a step shows nothing; GTK says on the error
// output what else went wrong, which fails the smoke test.
void check_sample_enclosure_dialog(GuiState& state);

// ---- Register Archive (register_wizard.cpp) and the comparison (compare_dialog.cpp)

// Opens Register Archive on its first page, Choose the source: the sidebar's
// + button, Register Archive in Archives, and Register the first archive. It
// reads the attached volumes off the window's thread: in the screenshot
// mode, a fixed sample list instead (sample_volumes). Says in a message why
// it does not open: no repository is open.
void open_register_wizard(GuiState& state);

// Create a new revision... of an archive's details: Register Archive for
// `archive`, once the attached volumes were read, with the folder or volume
// where they hold it now (drive letters follow the order in which devices
// are attached), or else `root`, the one its latest revision recorded, read
// as Next reads it; then on its Options page with Create a new revision on,
// not started. On its first page, saying why, where that root cannot be
// registered now or another archive is there.
void register_new_revision(GuiState& state, const Uuid& archive, const std::string& root);

// Register this volume of Devices: Register Archive on its first page with
// the volume at `root` (its mount point or GUID path) chosen, once the
// attached volumes were read, ready for Next.
void register_volume(GuiState& state, const std::string& root);

// Register as another archive... of the enclosure question (decision 0047):
// Register Archive for `root`, where the filesystem of `archive` is on
// another device (most likely a sector-by-sector clone), read at once; then
// on its Options page as a separate archive (--force-new-archive), a copy of
// `archive`, not started.
void register_as_copy(GuiState& state, const std::string& root, const Uuid& archive);

// The comparison dialog with `reference` as A and `copy` as B, where the
// repository lists them; without `copy`, B is a copy of A, or another
// archive. Not started: Compare starts its job. Says in a message why it does
// not open: no repository is open.
void compare_with(GuiState& state, const Uuid& reference, const std::optional<Uuid>& copy);

// An open comparison dialog lists the archives that Archives read last,
// keeping A and B where they are listed.
void refresh_compare_dialog(GuiState& state);

// ---- Relocation, export, retirement (relocate_dialog.cpp, export_dialog.cpp,
// retire_dialog.cpp; decision 0046) ------------------------------------------

// "Relocate to a new carrier..." of an archive's details, and the button of
// its notice where its filesystem is on another device than recorded
// (MIGRATED), which gives the root found there as `preset_root`: the
// relocation dialog for `archive`, not started; where the latest read of the
// unfinished operations lists a stopped relocation of the archive, the dialog
// that resumes it, its root preset (`preset_root`, or as its record names
// it). It reads the attached volumes off the window's thread: in the
// screenshot mode, a fixed sample list instead (sample_volumes). Says in a
// message why it does not open: no repository is open, a job of the window
// waits or works on the archive, or its unfinished relocation cannot be
// resumed now (relocation_choice).
void open_relocation(GuiState& state, const Uuid& archive, const std::optional<std::string>& preset_root = {});

// Resume on a stopped relocation under Unfinished operations, and in its
// archive's details: continues it as `archive relocate <uuid> <root>
// --resume` does (app::relocation_resume), as a job shown in its archive's
// details, which it brings into view. The caller has made sure that a
// repository is open and that no job of the window reads it
// (refused_while_a_job_reads).
void resume_relocation(GuiState& state, const app::UnfinishedOperation& relocation);

// "Export manifest...": asks for a folder (GtkFileDialog), then exports the
// archive's latest manifest and identity.json into a folder of its own in it
// (export_folder_name), as a job with a bar of the bytes copied, which Stop
// cancels (R-94); refused in a message where the folder lies in the root of
// an attached archive (export_refusal).
void export_manifest(GuiState& state, const Uuid& archive);

// "Retire archive...": reads what retiring it would do (retirement_of) as
// the window's single step, then asks (AdwAlertDialog, Cancel the default):
// what retiring means, and for a lost manifest the command line's explanation
// with an explicit confirmation; Retire retires it as the window's step.
// Refused in a message while a job of the window waits or works on the
// archive, while a step runs, or as the engine refuses it (an unfinished
// operation that would give the archive a new revision, in the window's
// words).
void ask_to_retire(GuiState& state, const Uuid& archive);

// Closes Register Archive, the comparison dialog, the relocation dialog and
// the enclosure question at once, whatever they show (a language change
// replaces the window they belong to; the screenshot tour, before its next
// picture).
void close_dialogs(GuiState& state);

// History with its filter set to `archive`, its runs listed.
void show_history_of(GuiState& state, const Uuid& archive);

// The Settings language row. show_language_in_use selects the language in
// use. lock_language makes the row unavailable while a job waits or works, or
// a step of the window runs, saying so under its title, and available again
// once none does, because a language change rebuilds the window they report
// into.
void show_language_in_use(GuiState& state);
void lock_language(GuiState& state);

// Opens the repository (or initialises it on first run) and refreshes every view.
void open_repository_session(GuiState& state, bool initialise);

// Makes the open of open_repository_session(state, initialise) with the
// window shown, when that open will upgrade the repository's database
// (decision 0032): it keeps a copy of the database first, which takes a while
// for a large repository, and an open while another process upgrades it waits
// for that process. The window shows a notice in place of its views, with a
// progress bar that moves, while the open runs on the job's thread; once it is
// done, the window shows its views, Settings first, which says where the copy
// is, or, in place of the notice, why no repository is open. Returns false,
// having done nothing, when that open will not upgrade the database, while a
// job runs, and in the screenshot mode, whose sample repository is new.
bool open_with_upgrade_notice(GuiState& state, bool initialise);

// Builds the window and shows it. With `smoke_test` nothing is shown: the
// window is built, rebuilt in the other language and back (as a language
// change does), and the program quits. With state.screenshot_folder set, the
// screenshot tour runs once the window is shown.
void activate(GuiState& state, bool smoke_test);

// The documentation screenshots (screenshots.cpp): walks through the views,
// saves each one as a PNG in state.screenshot_folder and quits; exit status 1
// and a message on stderr when something fails.
void start_screenshot_tour(GuiState& state);

// Makes GTK's own texts (dialog buttons, ...) follow the interface language.
void use_gtk_language(app::Language language);

// Switches the interface language: remembers it in the config file, then
// rebuilds the window in the new language and shows it, on Settings. While
// a read of the attached devices is in flight, once it is back
// (DevicesPoll::language_after).
void change_language(GuiState& state, app::Language language);

// Replaces the window with one in `language`, keeping what the user typed,
// chose and was shown: the page shown, the details of jobs open; nothing is
// saved. `show` presents the new window (the smoke test rebuilds without
// showing anything).
void rebuild_window(GuiState& state, app::Language language, bool show);

} // namespace va::gui
