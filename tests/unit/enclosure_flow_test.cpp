// What the window asks and says of an archive's device seen behind another
// enclosure (decision 0047): the enclosure case of Archives
// (src/gui/device_rows.*), the steps of the enclosure question and what each
// answer leads to (src/gui/enclosure_flow.*), the reads of a device's own
// serial that the window keeps and why one gave nothing
// (src/gui/device_reads.*), Devices' and Register Archive's offer of a read
// (src/gui/device_rows.*, src/gui/register_choices.*), the result page and
// History of a run that went on with a confirmation, and the screenshot
// mode's sample device identity reader (src/gui/displayed_identity.*).
// Every volume, device, enclosure, serial, archive and root below is sample
// data made up for the test (SAMPLE-* models, serials, WWNs and system names,
// ARCH-SAMPLE-* labels, c0ffee00-... uuids, mount points under
// /media/SAMPLE-*); none is of a real volume, device or archive. No read asks
// Windows anything: the reads are made-up results.
#include "test_support.h"

#include "app/enclosure_words.h"
#include "common/utf8.h"
#include "core/engine/carrier_confirmation.h"
#include "core/identity/volume_json.h"
#include "core/smart/smart_data.h"
#include "gui/device_reads.h"
#include "gui/device_rows.h"
#include "gui/displayed_identity.h"
#include "gui/enclosure_flow.h"
#include "gui/history_rows.h"
#include "gui/register_choices.h"
#include "gui/run_result.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using va::CarrierConfirmation;
using va::DeviceIdentityFailure;
using va::IoError;
using va::IoErrorKind;
using va::app::Language;
using va::app::Text;
using va::gui::EnclosureNext;
using va::gui::EnclosureStep;
using va::gui::IdentityShown;
namespace answer = va::gui::enclosure_answer;

const va::Uuid archive = "c0ffee00-0000-4000-8000-000000004701";
const va::Uuid other_archive = "c0ffee00-0000-4000-8000-000000004702";
const std::string enclosure_recorded = "SAMPLE-ENCLOSURE-A";
const std::string enclosure_now = "SAMPLE-ENCLOSURE-B";

va::PhysicalDeviceInfo enclosed_device(std::string serial, std::string bus = "usb")
{
    va::PhysicalDeviceInfo device;
    device.device_ref = "SAMPLE-DEVICE-REF-47";
    device.model = "SAMPLE-BRIDGE-MODEL";
    device.serial = std::move(serial);
    device.capacity_bytes = std::uint64_t{1} << 40;
    device.bus = std::move(bus);
    return device;
}

va::VolumeInfo volume(std::string mount_point, va::PhysicalDeviceInfo device)
{
    va::VolumeInfo info;
    info.mount_point = std::filesystem::path(mount_point);
    info.filesystem_type = "ntfs";
    info.filesystem_uuid = "c0ffee00-0000-4000-8000-0000000047f1";
    info.volume_label = "SAMPLE-ENCLOSED";
    info.capacity_bytes = std::uint64_t{1} << 40;
    info.devices = {std::move(device)};
    return info;
}

// The carrier as the archive's latest revision recorded it (no mount point,
// §B4.3), behind enclosure A.
va::VolumeInfo recorded_carrier()
{
    auto recorded = volume("", enclosed_device(enclosure_recorded));
    recorded.mount_point.clear();
    return recorded;
}

va::ArchiveStatus status_of(const va::Uuid& uuid, const va::VolumeInfo& identity, std::string state = "REGISTERED")
{
    va::ArchiveStatus status;
    status.archive.archive_uuid = uuid;
    status.archive.label = "ARCH-SAMPLE-47";
    status.archive.current_revision = 1;
    status.archive.status = std::move(state);
    va::ManifestHeader header;
    header.archive_uuid = uuid;
    header.label = status.archive.label;
    header.identity = identity;
    header.root_hint = "/media/SAMPLE-OLD/Photos";
    status.header = header;
    return status;
}

va::ArchiveCandidate candidate(const va::Uuid& uuid, va::VolumeInfo identity, std::string root_subpath)
{
    va::ArchiveCandidate found;
    found.archive_uuid = uuid;
    found.identity = std::move(identity);
    found.root_subpath = std::move(root_subpath);
    return found;
}

// A read of the attached devices that finds the archive's filesystem at
// /media/SAMPLE-ENC behind `now_device`.
va::gui::AttachedRead read_with(va::PhysicalDeviceInfo now_device)
{
    va::app::AttachedDevices attached;
    attached.volumes = {volume("/media/SAMPLE-ENC", std::move(now_device))};
    attached.devices = va::app::devices_of(attached.volumes);
    const std::vector<va::ArchiveCandidate> candidates{candidate(archive, recorded_carrier(), "Photos")};
    return va::gui::attached_read(attached, {}, candidates, true, std::nullopt);
}

va::DeviceIdentity sample_device(std::string serial, std::string wwn = "")
{
    return va::DeviceIdentity{"SAMPLE-OWN-MODEL", std::move(serial), std::move(wwn)};
}

va::DeviceIdentityRead sample_read(std::string serial)
{
    return va::DeviceIdentityRead{sample_device(std::move(serial)), "sat", "2026-11-05T00:00:00Z",
        "SAMPLE-DEVICE-REF-47"};
}

va::app::EnclosureFacts facts(std::optional<va::ConfirmedDevice> confirmed)
{
    return va::app::enclosure_facts("ARCH-SAMPLE-47", recorded_carrier(),
        volume("/media/SAMPLE-ENC", enclosed_device(enclosure_now)), std::move(confirmed));
}

const va::ConfirmedDevice confirmed_device{sample_device("SAMPLE-OWN-SERIAL-1"), "2026-11-01T00:00:00Z", ""};

std::vector<std::string> ids(const std::vector<va::gui::DialogAnswer>& answers)
{
    std::vector<std::string> shown;
    for (const auto& given : answers) {
        shown.push_back(given.id);
    }
    return shown;
}

std::string field(const std::vector<va::gui::Field>& fields, const std::string& name)
{
    const auto found = std::ranges::find(fields, name, &va::gui::Field::name);
    return found != fields.end() ? found->value : std::string("(none)");
}

void the_window_keeps_a_read_while_the_device_is_the_same()
{
    const auto device = enclosed_device(enclosure_now);
    CHECK(va::gui::still_the_device(device, device));
    auto other = device;
    other.serial = "SAMPLE-ENCLOSURE-C";  // another enclosure at the same system name
    CHECK(!va::gui::still_the_device(device, other));
    other = device;
    other.capacity_bytes /= 2;            // another device given the same name
    CHECK(!va::gui::still_the_device(device, other));
    auto unnamed = device;
    unnamed.device_ref.clear();
    CHECK(!va::gui::still_the_device(unnamed, unnamed));

    va::gui::KeptDeviceReads reads;
    reads[device.device_ref] = va::gui::KeptDeviceRead{sample_read("SAMPLE-OWN-SERIAL-1"), device, {archive}};
    CHECK(va::gui::kept_read_of(reads, device) != nullptr);
    CHECK(va::gui::kept_read_of(reads, other) == nullptr);
    // A read of the attached devices that lists it as it was keeps it; one
    // without it drops it.
    va::gui::keep_reads_of(reads, {device});
    CHECK(reads.size() == 1);
    va::gui::keep_reads_of(reads, {other});
    CHECK(reads.empty());

    // A read is offered only behind an enclosure that hides the device's own
    // serial, for a device the system names, where a reader is available.
    auto hiding = device;
    hiding.enclosure_hides_identity = true;
    CHECK(va::gui::offers_device_read(hiding, true));
    CHECK(!va::gui::offers_device_read(hiding, false));
    CHECK(!va::gui::offers_device_read(device, true));
    hiding.device_ref.clear();
    CHECK(!va::gui::offers_device_read(hiding, true));
}

void a_read_that_gave_nothing_says_why()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    using va::gui::ReadShownIn;
    // Cancelled: nothing is said.
    CHECK(!va::gui::read_problem(IoError{IoErrorKind::Cancelled, 0, "cancelled"}));
    const auto declined = va::gui::read_problem(IoError{IoErrorKind::PermissionDenied, 1223, "declined"});
    CHECK(declined && declined->failure == DeviceIdentityFailure::Declined && declined->cause.empty());
    const auto not_passed = va::gui::read_problem(IoError{IoErrorKind::CommandNotPassed, 0, "not passed"});
    CHECK(not_passed && not_passed->failure == DeviceIdentityFailure::NotPassed);
    const auto gone =
        va::gui::read_problem(IoError{IoErrorKind::NotFound, 0, "the device is no longer attached: SAMPLE-REF"});
    CHECK(gone && gone->failure == DeviceIdentityFailure::Unreadable &&
          gone->cause == "the device is no longer attached: SAMPLE-REF");

    // In the question: the words of decision 0047, which offer the operator
    // confirmation; never the advice to attach the device directly.
    CHECK(va::gui::read_problem_words(chinese, *declined, ReadShownIn::Question) == va::app::device_read_declined(chinese));
    CHECK(va::gui::read_problem_words(chinese, *not_passed, ReadShownIn::Question) == va::app::device_read_failed(chinese));
    const auto unreadable = va::gui::read_problem_words(chinese, *gone, ReadShownIn::Question);
    CHECK(unreadable.contains("裝置已不再連接：SAMPLE-REF") && unreadable.contains("自行確認") &&
          !unreadable.contains("SATA"));
    // On Register Archive's Confirm: it registers without the serial.
    CHECK(va::gui::read_problem_words(chinese, *declined, ReadShownIn::Registration) ==
          "沒有取得系統管理員權限，裝置本身的序號不會記錄。可以不記錄直接開始，或回上一步。");
    CHECK(va::gui::read_problem_words(english, *not_passed, ReadShownIn::Registration).contains("start without "
                                                                                                "recording it"));
    // In a toast of Devices: what happened, no more.
    CHECK(va::gui::read_problem_words(chinese, *declined, ReadShownIn::Devices) == "沒有取得系統管理員權限，未讀取裝置本身的序號。");
    CHECK(va::gui::read_problem_words(chinese, *not_passed, ReadShownIn::Devices) ==
          "讀不到裝置本身的序號：這個外接盒不轉送 ATA 指令。");
    CHECK(va::gui::read_problem_words(english, *gone, ReadShownIn::Devices) ==
          "The device's own serial could not be read: the device is no longer attached: SAMPLE-REF");
    const va::gui::ReadProblem unavailable{DeviceIdentityFailure::NotAvailable, ""};
    CHECK(va::gui::read_problem_words(chinese, unavailable, ReadShownIn::Devices).contains("裝置身分讀取程式無法使用"));
    for (const auto where : {ReadShownIn::Question, ReadShownIn::Registration, ReadShownIn::Devices}) {
        for (const auto& problem : {*declined, *not_passed, *gone, unavailable}) {
            const auto words = va::gui::read_problem_words(chinese, problem, where);
            CHECK(!words.empty() && !words.contains("serial") && !words.contains("SATA"));
        }
    }
    CHECK(va::gui::read_serial(sample_device("", "SAMPLE-WWN-1")) == "SAMPLE-WWN-1");

    // Every message of the device identity reader that a read that gave
    // nothing shows as its cause reads in Chinese too (the platform's
    // messages, with sample values).
    for (const std::string message : {"the device is no longer attached: SAMPLE-REF",
             "the device is no longer attached (error 2)", "the device did not answer IDENTIFY DEVICE in time (error 121)",
             "the device identity reader could not open the device (error 5)",
             "the device identity reader did not finish in time",
             "the device changed while it was read: another device has its number now (SAMPLE-REF)",
             "the device identity reader wrote no result (exit 1)",
             "the device identity reader is missing: SAMPLE-FOLDER/veriarchive-device-identity.exe",
             "no device of this computer by its number: SAMPLE-REF", "no temporary folder: SAMPLE-CAUSE",
             "Windows gave no handle of the device identity reader it started",
             "the device identity reader wrote a result larger than it writes",
             "the device identity reader wrote no result that it writes",
             "the device identity reader wrote an error that it does not write",
             "the device identity reader wrote an identity that it does not write",
             "the serial read before is no longer kept, as the device was detached or changed since"}) {
        const auto words = va::gui::read_problem_words(
            chinese, va::gui::ReadProblem{DeviceIdentityFailure::Unreadable, message}, ReadShownIn::Question);
        CHECK(!words.contains("the ") && !words.contains("Windows gave") && !words.contains("temporary") &&
              !words.contains(" device"));
    }
}

void archives_ask_first_behind_another_enclosure()
{
    const Text chinese(Language::TraditionalChinese);
    const auto status = status_of(archive, recorded_carrier());

    // Behind another USB enclosure: the case, at its mount point.
    const auto read = read_with(enclosed_device(enclosure_now));
    const auto found = va::gui::enclosure_case(&read, status, IdentityShown::Recorded);
    CHECK(found && found->where == va::path_to_utf8(std::filesystem::path("/media/SAMPLE-ENC")) &&
          va::path_to_utf8(found->root) == va::path_to_utf8(std::filesystem::path("/media/SAMPLE-ENC") / "Photos") &&
          va::enclosure_serial(found->recorded) == enclosure_recorded &&
          va::enclosure_serial(found->now) == enclosure_now);
    // Never in the screenshot mode, nor for a retired archive, nor without a read.
    CHECK(!va::gui::enclosure_case(&read, status, IdentityShown::Sample));
    CHECK(!va::gui::enclosure_case(&read, status_of(archive, recorded_carrier(), "RETIRED"), IdentityShown::Recorded));
    CHECK(!va::gui::enclosure_case(nullptr, status, IdentityShown::Recorded));
    // An internal device in place of the recorded one is no enclosure's:
    // only a relocation accepts it.
    const auto internal = read_with(enclosed_device("SAMPLE-INTERNAL", "sata"));
    CHECK(!va::gui::enclosure_case(&internal, status, IdentityShown::Recorded));
    // ... unless the device seen now is behind an enclosure that hides it.
    auto hiding = enclosed_device(enclosure_now, "sata");
    hiding.enclosure_hides_identity = true;
    const auto hidden = read_with(hiding);
    CHECK(va::gui::enclosure_case(&hidden, status, IdentityShown::Recorded).has_value());

    // Archives: its row, the notice with Confirm and FULL verify..., no
    // relocation beside it, the tooltip and the fields of the group Device.
    const auto attached = va::gui::archive_attached(chinese, &read, status, IdentityShown::Recorded);
    CHECK(attached.line && attached.line->state == va::gui::Attached::Migrated &&
          attached.line->words == "外接盒與記錄中的不同");
    CHECK(attached.notice == found->where + " 的外接盒與記錄中的不同。驗證前會先請您確認是不是同一個裝置。");
    CHECK(attached.confirm_first && !attached.relocate_root);
    CHECK(attached.verify_tooltip && attached.verify_tooltip->contains("會先請您確認是不是同一個裝置"));
    CHECK(field(attached.fields, "外接盒的序號") == "SAMPLE-ENCLOSURE-A（記錄中）・這次 SAMPLE-ENCLOSURE-B");
    // Never that the device changed enclosure: it may be a clone.
    CHECK(field(attached.fields, "目前的根目錄").starts_with("與記錄中的不同：外接盒不同（"));
    CHECK(attached.verify_tooltip == "這個封存的外接盒與記錄中的不同：「完整驗證」會先請您確認是不是同一個裝置。");
    // The group Device: the recorded serial, the enclosure's, only once,
    // beside the one seen now.
    const auto recorded_fields = va::gui::device_fields(chinese, status, IdentityShown::Recorded);
    CHECK(field(recorded_fields, "序號") == enclosure_recorded);
    const auto group = va::gui::details_device_fields(chinese, recorded_fields, attached);
    CHECK(field(group, "序號") == "(none)" && field(group, "外接盒的序號").starts_with("SAMPLE-ENCLOSURE-A（記錄中）") &&
          field(group, "型號") == "SAMPLE-BRIDGE-MODEL");
    CHECK(group.size() == recorded_fields.size() - 1 + attached.fields.size());
    // Not the enclosure case: the relocation of decision 0046, as before.
    const auto moved = va::gui::archive_attached(chinese, &internal, status, IdentityShown::Recorded);
    CHECK(!moved.confirm_first && moved.relocate_root && moved.line && moved.line->words == "裝置與記錄中的不同");
    CHECK(field(va::gui::details_device_fields(chinese, recorded_fields, moved), "序號") == enclosure_recorded);
    // Nor in the screenshot mode.
    CHECK(!va::gui::archive_attached(chinese, &read, status, IdentityShown::Sample).confirm_first);
}

void the_question_and_its_answers()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto without = facts(std::nullopt);
    const auto with = facts(confirmed_device);
    const std::string where = "SAMPLE:";
    using va::gui::EnclosureFlow;
    using va::gui::SerialRead;
    constexpr auto offered = SerialRead::Offered;

    // A read is offered only of a device whose enclosure hides its serial,
    // where this computer has a reader; of one that reports its own serial,
    // a read would only repeat it.
    auto hiding = enclosed_device(enclosure_now);
    hiding.enclosure_hides_identity = true;
    CHECK(va::gui::serial_read_offer(hiding, true) == SerialRead::Offered);
    CHECK(va::gui::serial_read_offer(hiding, false) == SerialRead::NoReader);
    CHECK(va::gui::serial_read_offer(enclosed_device(enclosure_now), true) == SerialRead::ReportedByDevice);
    CHECK(va::gui::serial_read_offer(enclosed_device(enclosure_now), false) == SerialRead::NoReader);
    auto unnamed = hiding;
    unnamed.device_ref.clear();
    CHECK(va::gui::serial_read_offer(unnamed, true) == SerialRead::NoReader);

    // Without a confirmed device: what changed, the two enclosures' serials
    // listed after it, the clone explanation, what Windows asks, Cancel /
    // Confirm without reading the serial... / Read the device's own serial
    // (the shield, suggested); Cancel first, the default.
    const auto asked = va::gui::enclosure_dialog(chinese, without, where, EnclosureFlow{}, offered);
    CHECK(asked.heading == "SAMPLE: 的外接盒與記錄中的不同");
    CHECK(asked.paragraphs == va::app::enclosure_question_body(chinese, without));
    CHECK((asked.fields ==
           std::vector<va::gui::Field>{{"記錄中", "SAMPLE-ENCLOSURE-A"}, {"這次", "SAMPLE-ENCLOSURE-B"}}));
    CHECK(asked.fields_after == 1 && !asked.paragraphs.front().contains("SAMPLE-ENCLOSURE"));
    CHECK(asked.note.contains("veriarchive-device-identity.exe") && asked.note.contains("不明發行者"));
    CHECK((ids(asked.answers) == std::vector<std::string>{"cancel", "without-serial", "read"}));
    CHECK(asked.answers[0].label == "取消" && !asked.answers[0].suggested);
    CHECK(asked.answers[1].label == "不讀序號，自行確認…" && !asked.answers[1].reads);
    CHECK(asked.answers[2].label == "讀取裝置本身的序號" && asked.answers[2].suggested && asked.answers[2].reads);
    CHECK(!asked.problem && !asked.reading);
    // With one: reading the serial tells; no operator confirmation offered,
    // until a read gave nothing.
    const auto known = va::gui::enclosure_dialog(chinese, with, where, EnclosureFlow{}, offered);
    CHECK((ids(known.answers) == std::vector<std::string>{"cancel", "read"}));
    CHECK(known.paragraphs.back().contains("SAMPLE-OWN-SERIAL-1"));
    const auto denied = va::gui::enclosure_dialog(chinese, with, where,
        EnclosureFlow{EnclosureStep::Question, std::nullopt, va::gui::ReadProblem{DeviceIdentityFailure::Declined, ""}},
        offered);
    CHECK((ids(denied.answers) == std::vector<std::string>{"cancel", "without-serial", "read"}));
    CHECK(denied.problem == va::app::device_read_declined(chinese));
    // Where no read can be made (Linux, a reader missing): no read, no
    // question of Windows; the operator alone confirms, a clone passing too,
    // and nothing says that a read would tell.
    const auto unread = va::gui::enclosure_dialog(english, with, where, EnclosureFlow{}, SerialRead::NoReader);
    CHECK((ids(unread.answers) == std::vector<std::string>{"cancel", "without-serial"}));
    CHECK(!unread.note.contains("Windows") && unread.note.contains("only you can confirm"));
    CHECK((unread.paragraphs ==
           std::vector<std::string>{va::app::enclosure_question_body(english, with).front(),
               va::app::sector_clone_words(english)}));
    CHECK(std::ranges::none_of(unread.paragraphs, [](const std::string& line) {
        return line.contains("SAMPLE-OWN-SERIAL-1") || line.contains("label");
    }));
    CHECK(unread.fields.size() == 2);
    // A device that reports its own serial: no read either, saying why.
    const auto reported = va::gui::enclosure_dialog(chinese, without, where, EnclosureFlow{}, SerialRead::ReportedByDevice);
    CHECK((ids(reported.answers) == std::vector<std::string>{"cancel", "without-serial"}));
    CHECK(reported.note.starts_with("這個裝置會透過外接盒回報自己的序號") && !reported.note.contains("Windows"));
    CHECK(reported.paragraphs.size() == 2 && reported.paragraphs.back() == va::app::sector_clone_words(chinese));

    // While the reader waits and reads: Cancel only, a spinner, what to answer.
    const auto reading = va::gui::enclosure_dialog(
        chinese, without, where, EnclosureFlow{EnclosureStep::Reading, std::nullopt, std::nullopt}, offered);
    CHECK(reading.reading && (ids(reading.answers) == std::vector<std::string>{"cancel"}));
    CHECK(reading.paragraphs.front().contains("使用者帳戶控制") &&
          reading.paragraphs.front().contains("veriarchive-device-identity.exe"));

    // The operator confirmation: a clone passes too; the history records it.
    const auto operator_step = va::gui::enclosure_dialog(chinese, without, where,
        EnclosureFlow{EnclosureStep::OperatorConfirmation, std::nullopt, std::nullopt}, offered);
    CHECK(operator_step.heading == "不讀序號，自行確認");
    CHECK(operator_step.paragraphs.back().contains("複本也會通過驗證") &&
          operator_step.paragraphs.back().contains("歷程都會記錄"));
    CHECK((ids(operator_step.answers) == std::vector<std::string>{"cancel", "confirmed"}));
    CHECK(operator_step.answers[1].label == "是同一個裝置，開始完整驗證" && !operator_step.answers[1].suggested);

    // What the device reports: model, serial, WWN, compared with the label;
    // neither answer suggested.
    const auto device = sample_device("SAMPLE-OWN-SERIAL-2", "SAMPLE-WWN-2");
    const auto read_now = va::gui::enclosure_dialog(
        chinese, without, where, EnclosureFlow{EnclosureStep::DeviceRead, device, std::nullopt}, offered);
    CHECK(read_now.heading == "讀到的裝置");
    CHECK((read_now.fields == std::vector<va::gui::Field>{{"型號", "SAMPLE-OWN-MODEL"},
                                  {"裝置本身的序號", "SAMPLE-OWN-SERIAL-2"}, {"WWN", "SAMPLE-WWN-2"}}));
    CHECK(read_now.paragraphs.front().contains("請對照裝置外殼標籤上的序號"));
    CHECK((ids(read_now.answers) == std::vector<std::string>{"cancel", "other", "same"}));
    CHECK(std::ranges::none_of(read_now.answers, &va::gui::DialogAnswer::suggested));
    CHECK(read_now.answers[1].label == "不是，是另一個裝置");

    // Another device: the serial recorded and the one read, Register as
    // another archive...; "Another device" where none was recorded.
    const auto not_mine = va::gui::enclosure_dialog(
        chinese, with, where, EnclosureFlow{EnclosureStep::OtherDevice, device, std::nullopt}, offered);
    CHECK(not_mine.heading == "這不是 ARCH-SAMPLE-47 的裝置");
    CHECK((not_mine.fields ==
           std::vector<va::gui::Field>{{"記錄的序號", "SAMPLE-OWN-SERIAL-1"}, {"這次的序號", "SAMPLE-OWN-SERIAL-2"}}));
    CHECK((ids(not_mine.answers) == std::vector<std::string>{"cancel", "register"}));
    CHECK(not_mine.answers[1].label == "註冊為另一個封存…" && not_mine.answers[1].suggested);
    const auto another = va::gui::enclosure_dialog(
        chinese, without, where, EnclosureFlow{EnclosureStep::OtherDevice, device, std::nullopt}, offered);
    CHECK(another.heading == "是另一個裝置" && another.fields.empty());
    CHECK(another.paragraphs.back() == "沒有記錄任何內容。");
}

void what_each_answer_and_read_leads_to()
{
    const auto without = facts(std::nullopt);
    const auto with = facts(confirmed_device);
    using va::gui::EnclosureFlow;
    using Then = EnclosureNext::Then;

    // After a read: the confirmed device's serial verifies by it, at once;
    // another's is not this archive's; with none, the operator compares.
    const auto same = va::gui::after_read(with, sample_read("SAMPLE-OWN-SERIAL-1"));
    CHECK(same.then == Then::Verify && same.confirmation &&
          same.confirmation->how == CarrierConfirmation::How::DeviceSerial && same.confirmation->device &&
          same.confirmation->device->serial == "SAMPLE-OWN-SERIAL-1" &&
          same.confirmation->enclosure_recorded == enclosure_recorded &&
          same.confirmation->enclosure_now == enclosure_now);
    const auto other = va::gui::after_read(with, sample_read("SAMPLE-OWN-SERIAL-9"));
    CHECK(other.then == Then::Show && other.flow.step == EnclosureStep::OtherDevice && !other.confirmation);
    const auto compare = va::gui::after_read(without, sample_read("SAMPLE-OWN-SERIAL-2"));
    CHECK(compare.then == Then::Show && compare.flow.step == EnclosureStep::DeviceRead && compare.flow.read &&
          compare.flow.read->serial == "SAMPLE-OWN-SERIAL-2");
    // A read that gave nothing: back to the question, saying why; cancelled:
    // closed, nothing said.
    const auto declined = va::gui::after_read(
        with, std::unexpected(IoError{IoErrorKind::PermissionDenied, 1223, "declined"}));
    CHECK(declined.then == Then::Show && declined.flow.step == EnclosureStep::Question && declined.flow.problem &&
          declined.flow.problem->failure == DeviceIdentityFailure::Declined);
    const auto failed =
        va::gui::after_read(without, std::unexpected(IoError{IoErrorKind::CommandNotPassed, 0, "not passed"}));
    CHECK(failed.flow.step == EnclosureStep::Question && failed.flow.problem &&
          failed.flow.problem->failure == DeviceIdentityFailure::NotPassed);
    CHECK(va::gui::after_read(with, std::unexpected(IoError{IoErrorKind::Cancelled, 0, "cancelled"})).then ==
          Then::Close);

    // The answers. The question always opens on its first step
    // (EnclosureFlow{}), whose read reads the device anew: no read kept from
    // before verifies by the serial on its own.
    const EnclosureFlow question{};
    const auto reads = va::gui::answered(without, question, answer::read);
    CHECK(reads.then == Then::Read && reads.flow.step == EnclosureStep::Reading);
    const auto asks = va::gui::answered(without, question, answer::without_serial);
    CHECK(asks.then == Then::Show && asks.flow.step == EnclosureStep::OperatorConfirmation && !asks.flow.read);
    const auto confirmed = va::gui::answered(without, asks.flow, answer::confirmed);
    CHECK(confirmed.then == Then::Verify && confirmed.confirmation &&
          confirmed.confirmation->how == CarrierConfirmation::How::Operator && !confirmed.confirmation->device);
    // "Same" after a read: the operator's, with the device read (as the
    // engine records it then).
    const auto by_operator = va::gui::answered(without, compare.flow, answer::same);
    CHECK(by_operator.then == Then::Verify && by_operator.confirmation &&
          by_operator.confirmation->how == CarrierConfirmation::How::Operator && by_operator.confirmation->device &&
          by_operator.confirmation->device->serial == "SAMPLE-OWN-SERIAL-2");
    const auto not_same = va::gui::answered(without, compare.flow, answer::other);
    CHECK(not_same.then == Then::Show && not_same.flow.step == EnclosureStep::OtherDevice);
    CHECK(va::gui::answered(without, not_same.flow, answer::register_it).then == Then::Register);
    CHECK(va::gui::answered(without, question, answer::cancel).then == Then::Close);
    // An answer only in the step that offers it: never "same" without a read,
    // nor a verification from the question itself.
    CHECK(va::gui::answered(without, question, answer::same).then == Then::Close);
    CHECK(va::gui::answered(without, question, answer::confirmed).then == Then::Close);
    CHECK(va::gui::answered(without, EnclosureFlow{EnclosureStep::DeviceRead, std::nullopt, std::nullopt},
              answer::same)
              .then == Then::Close);
    CHECK(va::gui::answered(without, reads.flow, answer::read).then == Then::Close);
}

void a_confirmed_verification_says_what_it_records()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    const auto without = facts(std::nullopt);
    const auto unread = va::app::confirmation_by_operator(without, std::nullopt);
    CHECK(va::gui::confirmed_running_notice(chinese, "ARCH-SAMPLE-47", unread) ==
          "已確認是 ARCH-SAMPLE-47 的裝置（由您確認、未讀序號）。這次完整驗證通過之後，才會記下新的外接盒 "
          "SAMPLE-ENCLOSURE-B；無論結果如何，歷程都會記錄這次的確認方式。");
    const auto compared = va::app::confirmation_by_operator(without, sample_device("SAMPLE-OWN-SERIAL-2"));
    CHECK(va::gui::confirmed_running_notice(chinese, "ARCH-SAMPLE-47", compared)
              .contains("新的外接盒 SAMPLE-ENCLOSURE-B 與裝置本身的序號 SAMPLE-OWN-SERIAL-2"));
    const auto by_serial = va::app::confirmation_by_serial(facts(confirmed_device), sample_device("SAMPLE-OWN-SERIAL-1"));
    CHECK(va::gui::confirmed_running_notice(english, "ARCH-SAMPLE-47", by_serial) ==
          "Confirmed as the device of ARCH-SAMPLE-47 (the device's serial matches). Only once this FULL verify passes "
          "is the new enclosure SAMPLE-ENCLOSURE-B recorded; whatever the result, the history records how it was "
          "confirmed.");

    // The end toast: what it recorded, or that it recorded nothing.
    const auto base = va::gui::verified_toast(chinese, "ARCH-SAMPLE-47", "PASSED");
    va::VerifyOutcome outcome;
    CHECK(va::gui::confirmed_toast(chinese, base, outcome) == base);
    outcome.carrier_confirmed = va::RecordedConfirmation{unread, 1};
    outcome.carrier_revision = 2;
    CHECK(va::gui::confirmed_toast(chinese, base, outcome) ==
          base + "・已記下新的外接盒：由版次 2 記錄（由您確認、未讀序號）");
    outcome.carrier_revision = 0;
    outcome.carrier_not_recorded = "the verification did not pass";
    CHECK(va::gui::confirmed_toast(chinese, va::gui::verified_toast(chinese, "ARCH-SAMPLE-47", "FAILED"), outcome)
              .ends_with("・未記下新的外接盒：驗證未通過"));
    // Stopped: nothing recorded yet, nothing said.
    outcome.carrier_not_recorded.clear();
    CHECK(va::gui::confirmed_toast(chinese, base, outcome) == base);

    CHECK(va::gui::enclosure_stop_words(chinese, "SAMPLE:") ==
          "未能驗證：SAMPLE: 的外接盒與記錄中的不同（已遷移）。在封存的詳細資料中按「完整驗證」，會先請您確認是不是同一個"
          "裝置，再驗證它。");

    // Stopped, it says that only a pass once it is resumed records the new
    // enclosure.
    va::app::OperationRow stopped;
    stopped.progress = "SAMPLE 50%";
    CHECK(va::gui::stopped_verification_words(chinese, stopped, false, true) ==
          "此封存上次的完整驗證停止於 SAMPLE 50%。按「繼續」即可接續，已讀過的不必重讀。繼續之後若通過，才會記下新的外接盒。");
    CHECK(va::gui::stopped_verification_words(english, stopped, false, true)
              .ends_with("Only once it is resumed and passes is the new enclosure recorded."));
    CHECK(!va::gui::stopped_verification_words(chinese, stopped, false).contains("外接盒"));
}

void the_result_page_and_history_show_the_confirmation()
{
    const Text chinese(Language::TraditionalChinese);
    const auto confirmation = va::app::confirmation_by_operator(facts(std::nullopt), std::nullopt);
    va::VerifyRunRecord run;
    run.run_id = "c0ffee00-0000-4000-8000-0000000047a1";
    run.archive_uuid = archive;
    run.revision = 2;
    run.mode = "FULL";
    run.started = "2026-11-05T01:00:00Z";
    run.finished = "2026-11-05T02:00:00Z";
    run.result = "PASSED";
    run.coverage_bytes = 1024;
    nlohmann::json counts{{"Verified", 3}};
    counts[va::carrier_confirmed_member] = va::carrier_confirmation_json(confirmation, 1);
    run.counts_json = counts.dump();
    run.host = "SAMPLE-HOST";
    run.app_version = "0.0.0-sample";
    va::gui::RecordedRun recorded;
    recorded.run = run;
    recorded.history = {run};
    recorded.evidence = va::EvidenceRecord{archive, run.run_id, run.finished,
        va::volume_to_json_text(volume("/media/SAMPLE-ENC", enclosed_device(enclosure_now))), "MIGRATED",
        "enclosure changed, confirmed by the operator without the serial"};
    recorded.label = "ARCH-SAMPLE-47";
    const auto page = va::gui::result_page(chinese, recorded, IdentityShown::Recorded);
    CHECK(field(page.identity, "媒體身分") == "已遷移：外接盒與記錄中的不同（由您確認、未讀序號）");
    CHECK(field(page.technical, "備註") == "外接盒已更換，由您確認、未讀序號");

    const auto rows = va::gui::history_rows(chinese, {run}, {{archive, "ARCH-SAMPLE-47", "SAMPLE-ENCLOSED"}}, std::nullopt);
    CHECK(rows.size() == 1 && std::ranges::any_of(rows.front().marks, [](const va::gui::Pill& mark) {
        return mark.words == "外接盒已更換，由您確認、未讀序號";
    }));
}

void devices_offer_to_read_the_devices_own_serial()
{
    const Text chinese(Language::TraditionalChinese);
    auto hiding = enclosed_device(enclosure_now);
    hiding.enclosure_hides_identity = true;
    const auto read = read_with(hiding);
    const std::vector<va::gui::ArchiveName> names{{archive, "ARCH-SAMPLE-47", "SAMPLE-ENCLOSED"}};

    // With a reader: the enclosure's serial named so, then the button and
    // what Windows asks.
    auto cards = va::gui::device_cards(chinese, read, names, IdentityShown::Recorded, {}, {}, true);
    CHECK(cards.size() == 1 && cards.front().own);
    if (!cards.empty() && cards.front().own) {
        const auto& own = *cards.front().own;
        CHECK(field(cards.front().fields, "外接盒的序號") == enclosure_now);
        CHECK(own.device_ref == "SAMPLE-DEVICE-REF-47" && own.offered && !own.reading);
        CHECK(own.value == "這個外接盒不會轉送裝置本身的序號。" && own.note.contains("veriarchive-device-identity.exe"));
    }
    // Being read: a spinner in place of the button.
    cards = va::gui::device_cards(chinese, read, names, IdentityShown::Recorded, {}, {}, true, "SAMPLE-DEVICE-REF-47");
    CHECK(!cards.empty() && cards.front().own && cards.front().own->reading);
    // Without a reader (Linux): the enclosure note only.
    cards = va::gui::device_cards(chinese, read, names, IdentityShown::Recorded, {}, {}, false);
    CHECK(!cards.empty() && cards.front().own && !cards.front().own->offered && cards.front().own->note.empty() &&
          cards.front().own->value == "這個外接盒不會轉送裝置本身的序號。");
    // Read: the serial, and whose confirmed device it is, or that it is none's.
    va::gui::KeptDeviceReads reads;
    reads[hiding.device_ref] = va::gui::KeptDeviceRead{sample_read("SAMPLE-OWN-SERIAL-1"), hiding, {archive}};
    cards = va::gui::device_cards(chinese, read, names, IdentityShown::Recorded, {}, reads, true);
    CHECK(!cards.empty() && cards.front().own && !cards.front().own->offered &&
          cards.front().own->value == "SAMPLE-OWN-SERIAL-1（已讀取）・ARCH-SAMPLE-47 的裝置（已確認）");
    reads[hiding.device_ref].confirmed_of.clear();
    cards = va::gui::device_cards(chinese, read, names, IdentityShown::Recorded, {}, reads, true);
    // Not that it is no archive's device: one registered before decision
    // 0047 records no device's own serial.
    CHECK(!cards.empty() && cards.front().own &&
          cards.front().own->value == "SAMPLE-OWN-SERIAL-1（已讀取）・還沒有封存記錄這個序號");
    // Once a pass or a registration recorded it, the read says whose
    // confirmed device it is; a read of another device says nothing new.
    va::gui::note_confirmed(reads, sample_device("SAMPLE-OWN-SERIAL-9"), archive);
    CHECK(reads[hiding.device_ref].confirmed_of.empty());
    va::gui::note_confirmed(reads, sample_device("SAMPLE-OWN-SERIAL-1"), archive);
    va::gui::note_confirmed(reads, sample_device("SAMPLE-OWN-SERIAL-1"), archive);
    CHECK((reads[hiding.device_ref].confirmed_of == std::vector<va::Uuid>{archive}));
    cards = va::gui::device_cards(chinese, read, names, IdentityShown::Recorded, {}, reads, true);
    CHECK(!cards.empty() && cards.front().own &&
          cards.front().own->value == "SAMPLE-OWN-SERIAL-1（已讀取）・ARCH-SAMPLE-47 的裝置（已確認）");
    // A device that reports its own serial: nothing of the kind.
    const auto plain = read_with(enclosed_device(enclosure_now));
    cards = va::gui::device_cards(chinese, plain, names, IdentityShown::Recorded, {}, {}, true);
    CHECK(!cards.empty() && !cards.front().own && field(cards.front().fields, "序號") == enclosure_now);

    // Under its volume, the archive found behind a changed enclosure says
    // that the enclosure differs, as Archives says it; every other MIGRATED,
    // that the device does.
    const auto archive_title = [](const std::vector<va::gui::DeviceCard>& shown) {
        return shown.size() == 1 && shown.front().volumes.size() == 1 &&
                       shown.front().volumes.front().archives.size() == 1
                   ? std::optional(shown.front().volumes.front().archives.front())
                   : std::nullopt;
    };
    const std::vector<va::ArchiveStatus> statuses{status_of(archive, recorded_carrier())};
    const auto changed = va::gui::enclosure_cases(plain, statuses, IdentityShown::Recorded);
    CHECK((changed == std::vector<va::Uuid>{archive}));
    CHECK(va::gui::enclosure_cases(plain, statuses, IdentityShown::Sample).empty());
    const auto on_volume =
        archive_title(va::gui::device_cards(chinese, plain, names, IdentityShown::Recorded, {}, {}, true, {}, changed));
    CHECK(on_volume && on_volume->migrated &&
          on_volume->title == "檔案系統屬於 ARCH-SAMPLE-47，但外接盒與記錄中的不同（已遷移）");
    const auto english_volume = archive_title(
        va::gui::device_cards(Text(), plain, names, IdentityShown::Recorded, {}, {}, true, {}, changed));
    CHECK(english_volume && english_volume->title == "The filesystem belongs to ARCH-SAMPLE-47, but the enclosure "
                                                     "differs from the one recorded (MIGRATED)");
    const auto internal = read_with(enclosed_device("SAMPLE-INTERNAL", "sata"));
    CHECK(va::gui::enclosure_cases(internal, statuses, IdentityShown::Recorded).empty());
    const auto relocation = archive_title(va::gui::device_cards(chinese, internal, names, IdentityShown::Recorded,
        {}, {}, true, {}, va::gui::enclosure_cases(internal, statuses, IdentityShown::Recorded)));
    CHECK(relocation && relocation->migrated &&
          relocation->title == "檔案系統屬於 ARCH-SAMPLE-47，但裝置與記錄中的不同（已遷移）");
}

void register_archive_offers_to_record_the_serial()
{
    const Text english;
    const Text chinese(Language::TraditionalChinese);
    auto hiding = enclosed_device(enclosure_now);
    hiding.enclosure_hides_identity = true;
    const va::gui::ChosenSource source{
        std::filesystem::path("/media/SAMPLE-ENC/Photos"), volume("/media/SAMPLE-ENC", hiding), "Photos", std::nullopt};
    const va::gui::SourceOptions options;

    const auto offer = va::gui::device_serial_offer(source, options, IdentityShown::Recorded, true, {});
    CHECK(offer && offer->device.device_ref == "SAMPLE-DEVICE-REF-47" && !offer->kept);
    // Not without a reader, in the screenshot mode, for a resume, nor for a
    // device that reports its own serial.
    CHECK(!va::gui::device_serial_offer(source, options, IdentityShown::Recorded, false, {}));
    CHECK(!va::gui::device_serial_offer(source, options, IdentityShown::Sample, true, {}));
    auto resuming = options;
    resuming.unfinished = va::gui::PendingRoot{archive, "ARCH-SAMPLE-47", "/media/SAMPLE-ENC/Photos"};
    CHECK(!va::gui::device_serial_offer(source, resuming, IdentityShown::Recorded, true, {}));
    auto plain = source;
    plain.volume.devices.front().enclosure_hides_identity = false;
    CHECK(!va::gui::device_serial_offer(plain, options, IdentityShown::Recorded, true, {}));
    // A read kept from Devices, still of that device: recorded, no question.
    va::gui::KeptDeviceReads reads;
    reads[hiding.device_ref] = va::gui::KeptDeviceRead{sample_read("SAMPLE-OWN-SERIAL-1"), hiding, {}};
    const auto kept = va::gui::device_serial_offer(source, options, IdentityShown::Recorded, false, reads);
    CHECK(kept && kept->kept && kept->kept->device.serial == "SAMPLE-OWN-SERIAL-1");

    // The switch is off by default; the operator chooses.
    va::gui::OptionsChosen chosen;
    CHECK(!chosen.record_device_serial);
    const auto serial_field = [](std::string value) { return va::gui::Field{"裝置本身的序號", std::move(value)}; };
    CHECK(va::gui::device_serial_field(chinese, offer, chosen, false) == serial_field("不記錄"));
    chosen.record_device_serial = true;
    CHECK(va::gui::device_serial_field(chinese, offer, chosen, false) ==
          serial_field("開始時會詢問系統管理員權限，讀取並記下"));
    CHECK(va::gui::device_serial_field(chinese, offer, chosen, true) == serial_field("不記錄"));
    CHECK(va::gui::device_serial_field(chinese, kept, va::gui::OptionsChosen(), false) ==
          serial_field("SAMPLE-OWN-SERIAL-1（已讀取）"));
    CHECK(!va::gui::device_serial_field(chinese, std::nullopt, chosen, false));
    const auto note = va::gui::device_serial_note(chinese);
    CHECK(note.contains("不必再問您") && note.contains("veriarchive-device-identity.exe") && note.ends_with("開始註冊時才會詢問。"));
    CHECK(va::gui::recorded_device_serial_line(english, sample_read("SAMPLE-OWN-SERIAL-1")) ==
          "Device's own serial recorded: SAMPLE-OWN-SERIAL-1 (the archive's confirmed device from now on)");
}

void the_screenshot_mode_reads_a_sample_identity()
{
    // SAMPLE DATA: available, answers at once, never asks Windows.
    const auto reader = va::gui::sample_device_identity_helper();
    CHECK(reader->available());
    va::CancelToken cancel;
    const auto read = reader->read_device_identity("SAMPLE-DEVICE-REF-03", cancel);
    CHECK(read && read->device.serial == va::gui::sample_own_serial && read->device.model.starts_with("SAMPLE-") &&
          read->device.wwn.starts_with("SAMPLE-") && read->device_ref == "SAMPLE-DEVICE-REF-03" &&
          read->how == "sample");
    // Decision 0052: each sample device's own identity (that of a device
    // that hides none is the serial it reports) and a sample SMART read, of
    // which SAMPLE-DEVICE-02's alerts; any other device passes no command on.
    const auto all = reader->read_devices({"SAMPLE-DEVICE-REF-01", "SAMPLE-DEVICE-REF-02", "SAMPLE-ELSEWHERE"}, cancel);
    CHECK(all && all->size() == 3);
    if (all && all->size() == 3) {
        CHECK((*all)[0].identity && (*all)[0].identity->device.serial == "SAMPLE-SERIAL-01" && (*all)[0].smart &&
              !va::any_smart_alert(va::smart_findings(*(*all)[0].smart, nullptr)));
        CHECK((*all)[1].smart && va::any_smart_alert(va::smart_findings(*(*all)[1].smart, nullptr)));
        CHECK(!(*all)[2].identity && (*all)[2].identity.error().kind == IoErrorKind::CommandNotPassed &&
              !(*all)[2].smart);
    }
    cancel.cancel();
    const auto stopped = reader->read_device_identity("SAMPLE-DEVICE-REF-01", cancel);
    CHECK(!stopped && stopped.error().kind == IoErrorKind::Cancelled);
    // One sample device, whose health cannot be read, hides its own serial,
    // so Devices offers the read.
    const auto volumes = va::gui::sample_volumes();
    CHECK(std::ranges::count_if(volumes, [](const va::gui::SampleVolume& sample) {
        const auto& device = sample.volume.devices.front();
        return device.enclosure_hides_identity && device.device_ref == "SAMPLE-DEVICE-REF-03" &&
               device.serial == "SAMPLE-SERIAL-03" && (!device.health || device.health->status == va::HealthStatus::Unknown);
    }) == 1);
    CHECK(std::ranges::count_if(volumes, [](const va::gui::SampleVolume& sample) {
        return sample.volume.devices.front().enclosure_hides_identity;
    }) == 1);
}

} // namespace

void enclosure_flow_tests()
{
    the_window_keeps_a_read_while_the_device_is_the_same();
    a_read_that_gave_nothing_says_why();
    archives_ask_first_behind_another_enclosure();
    the_question_and_its_answers();
    what_each_answer_and_read_leads_to();
    a_confirmed_verification_says_what_it_records();
    the_result_page_and_history_show_the_confirmation();
    devices_offer_to_read_the_devices_own_serial();
    register_archive_offers_to_record_the_serial();
    the_screenshot_mode_reads_a_sample_identity();
}
