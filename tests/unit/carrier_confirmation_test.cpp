// Decision 0047: a FULL verification that goes on past a changed enclosure
// with a carrier confirmation, the records it leaves, the confirmed device, and
// the window's words for it. SAMPLE DATA ONLY: every enclosure, device, serial
// and WWN below is made up (SAMPLE-*, 5000c0ffee…), on the fake volume of the
// engine fixture and in a temporary repository; none is a real device's.
#include "test_support.h"

#include "engine_fixture.h"

#include "app/enclosure_words.h"
#include "app/history.h"
#include "app/text.h"
#include "common/utf8.h"
#include "core/engine/carrier_confirmation.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/relocate_engine.h"
#include "core/identity/volume_json.h"
#include "core/manifest/manifest_writer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {

using va::CarrierConfirmation;
using va::app::Language;
using va::app::Text;

constexpr const char* enclosure_a = "SAMPLE-ENCLOSURE-A";  // the enclosure at registration (sample)
constexpr const char* enclosure_b = "SAMPLE-ENCLOSURE-B";  // another enclosure, the same device in it (sample)
constexpr const char* enclosure_c = "SAMPLE-ENCLOSURE-C";  // yet another (sample)

va::DeviceIdentity sample_device(const std::string& serial, const std::string& wwn = "5000c0ffee000001")
{
    return va::DeviceIdentity{"SAMPLE Disk 4TB", serial, wwn};
}

// A sample read of the device's own identity, as the device identity reader
// would give it (made up).
va::DeviceIdentityRead sample_read(const std::string& serial)
{
    return va::DeviceIdentityRead{sample_device(serial), "sat", "2026-11-04T08:00:00Z", "SAMPLE-DEVICE-REF"};
}

// A sample archive on a device behind a USB enclosure, whose serial the
// system reports in place of the device's (all made up). `before` changes the
// fixture before the archive is registered.
struct Enclosed : test::EngineFixture {
    va::Uuid archive_uuid;

    explicit Enclosed(std::optional<va::DeviceIdentityRead> read = std::nullopt, std::string bus = "usb",
        const std::function<void(Enclosed&)>& before = {})
    {
        auto& device = media.volume.devices.front();
        device.model = "SAMPLE USB Enclosure";
        device.serial = enclosure_a;
        device.bus = std::move(bus);
        archive.add_file("SAMPLE-docs/report.txt", "sample report");
        archive.add_file("SAMPLE-photos/a.jpg", std::string(5000, 'a'));
        archive.add_file("SAMPLE-notes.txt", "sample notes");
        archive.add_directory("SAMPLE-empty");
        if (before) {
            before(*this);
        }
        va::RegisterRequest request;
        request.root = archive.root();
        request.device_read = std::move(read);
        const auto registered = register_root(request);
        CHECK(registered.has_value());
        if (registered) {
            archive_uuid = registered->archive_uuid;
        }
    }

    // The same device seen through another enclosure (sample).
    void move_to(const std::string& enclosure) { media.volume.devices.front().serial = enclosure; }

    std::expected<va::VerifyOutcome, va::VerifyStop> verify_by_uuid(
        std::optional<CarrierConfirmation> confirmation = std::nullopt)
    {
        va::VerifyRequest request;
        request.archive_uuid = archive_uuid;
        request.carrier_confirmation = std::move(confirmation);
        return verify(request);
    }

    // As verify_by_uuid, writing the repository through `writer` (a
    // test::StagingHookWriter that steps in before the record).
    std::expected<va::VerifyOutcome, va::VerifyStop> verify_by_uuid_through(
        va::IFileWriter& writer, CarrierConfirmation confirmation)
    {
        va::VerifyServices services{archive_files(), *repository.files, media, store, repository.context.repo,
            repository.context.repository_uuid, host, writer, processes};
        va::VerifyRequest request;
        request.archive_uuid = archive_uuid;
        request.carrier_confirmation = std::move(confirmation);
        return va::verify_full(services, request, cancel, progress);
    }

    std::expected<va::RelocateOutcome, va::RelocateFailure> relocate_here()
    {
        va::RelocateServices services{archive, *repository.files, *repository.writer, media, store,
            repository.context.repo, repository.context.repository_uuid, host, processes};
        va::RelocateRequest request;
        request.archive_uuid = archive_uuid;
        request.root = archive.root();
        return va::relocate_archive(services, request, cancel, progress);
    }

    std::vector<va::VerifyRunRecord> runs()
    {
        auto recorded = store.verify_runs(archive_uuid);
        CHECK(recorded.has_value());
        return recorded ? *recorded : std::vector<va::VerifyRunRecord>{};
    }

    std::uint32_t revision()
    {
        const auto archive_record = store.find_archive(archive_uuid);
        CHECK(archive_record && *archive_record);
        return archive_record && *archive_record ? (*archive_record)->current_revision : 0;
    }

    bool chain_holds()
    {
        const auto checked = store.check_history(repository.context.repository_uuid);
        return checked && !checked->first_broken_run && checked->runs > 0;
    }
};

CarrierConfirmation by_operator(const std::string& recorded, const std::string& now,
    std::optional<va::DeviceIdentity> read = std::nullopt)
{
    return CarrierConfirmation{CarrierConfirmation::How::Operator, std::move(read), recorded, now};
}

CarrierConfirmation by_serial(const std::string& recorded, const std::string& now, const va::DeviceIdentity& read)
{
    return CarrierConfirmation{CarrierConfirmation::How::DeviceSerial, read, recorded, now};
}

nlohmann::json counts_of(const va::VerifyRunRecord& run)
{
    return nlohmann::json::parse(run.counts_json, nullptr, false);
}

// The value at `pointer` ("/carrier_confirmed/how") in `json`, or null where
// there is none: a check that fails rather than throws.
nlohmann::json at(const nlohmann::json& json, const std::string& pointer)
{
    const nlohmann::json::json_pointer path(pointer);
    return json.contains(path) ? json.at(path) : nlohmann::json(nullptr);
}

// The pure rules: the enclosure's serial, which MIGRATED is a changed
// enclosure, the records' JSON both ways, and how two reads compare.
void enclosures_and_records()
{
    va::VolumeInfo usb_a = test::fake_volume("/vol");
    usb_a.devices.front().serial = enclosure_a;
    usb_a.devices.front().bus = "usb";
    auto usb_b = usb_a;
    usb_b.devices.front().serial = enclosure_b;
    CHECK(va::enclosure_serial(usb_a) == enclosure_a);
    auto wwn_only = usb_a;
    wwn_only.devices.front().serial.clear();
    wwn_only.devices.front().wwn = "5000c0ffee0000aa";
    CHECK(va::enclosure_serial(wwn_only) == "5000c0ffee0000aa");
    auto spanned = usb_a;
    spanned.devices.push_back(usb_b.devices.front());
    CHECK(va::enclosure_serial(spanned).empty());

    // Both behind a USB bridge; or the device seen now behind an enclosure that
    // hides its identity; never two internal devices, a device taken out of
    // its enclosure, or a spanned volume.
    CHECK(va::enclosure_changed(usb_a, usb_b));
    auto sata = usb_a;
    sata.devices.front().bus = "sata";
    CHECK(!va::enclosure_changed(sata, sata));
    CHECK(!va::enclosure_changed(usb_a, sata));
    CHECK(!va::enclosure_changed(sata, usb_b));
    auto hidden = usb_b;
    hidden.devices.front().enclosure_hides_identity = true;
    CHECK(va::enclosure_changed(sata, hidden));
    CHECK(!va::enclosure_changed(spanned, usb_b));
    CHECK(!va::enclosure_changed(usb_a, spanned));

    // The runtime-only mark is kept in raw evidence only where it is set, and
    // read back; 1.1.0, which knows no such member, ignores it.
    const auto hidden_json = va::volume_to_json(hidden);
    CHECK(at(hidden_json, "/devices/0/enclosure_hides_identity") == true);
    CHECK(!va::volume_to_json(usb_b)["devices"][0].contains("enclosure_hides_identity"));
    CHECK(va::volume_from_json(hidden_json).devices.front().enclosure_hides_identity);

    // A confirmation and its JSON, both ways.
    const auto operator_json = va::carrier_confirmation_json(by_operator(enclosure_a, enclosure_b), 4);
    CHECK(operator_json.dump() ==
          R"({"enclosure_now":"SAMPLE-ENCLOSURE-B","enclosure_recorded":"SAMPLE-ENCLOSURE-A","from_revision":4,)"
          R"("how":"operator"})");
    const auto serial_json =
        va::carrier_confirmation_json(by_serial(enclosure_a, enclosure_b, sample_device("SAMPLE-DEVICE-1")), 2);
    CHECK(serial_json["how"] == "device_serial" && at(serial_json, "/device/serial") == "SAMPLE-DEVICE-1" &&
          at(serial_json, "/device/model") == "SAMPLE Disk 4TB" && at(serial_json, "/device/wwn") == "5000c0ffee000001");
    const auto back = va::carrier_confirmation_from_json(serial_json);
    CHECK(back && back->from_revision == 2 && back->confirmation.how == CarrierConfirmation::How::DeviceSerial &&
          back->confirmation.device && back->confirmation.device->serial == "SAMPLE-DEVICE-1" &&
          back->confirmation.enclosure_recorded == enclosure_a && back->confirmation.enclosure_now == enclosure_b);
    CHECK(!va::carrier_confirmation_from_json(nlohmann::json{{"how", "override"}, {"from_revision", 1}}));
    CHECK(!va::carrier_confirmation_from_json(nlohmann::json{{"how", "operator"}}));
    CHECK(!va::carrier_confirmation_from_json(nlohmann::json{{"how", "device_serial"}, {"from_revision", 1}}));
    CHECK(!va::carrier_confirmation_from_json(nlohmann::json("operator")));

    // Two reads of one device: by serial (a byte-pair-swapped one too), or by WWN.
    CHECK(va::same_device_identity(sample_device("SAMPLE-DEVICE-1"), sample_device("SAMPLE-DEVICE-1", "")));
    CHECK(va::same_device_identity(sample_device("ASP-EM1L"), sample_device("SA-PMEL1", "")));
    CHECK(va::same_device_identity(sample_device("", "5000C0FFEE000001"), sample_device("", "5000c0ffee000001")));
    CHECK(!va::same_device_identity(sample_device("SAMPLE-DEVICE-1", "5000c0ffee000001"),
        sample_device("SAMPLE-DEVICE-2", "5000c0ffee000002")));
}

// §B10 as before: a changed enclosure without a confirmation stops as every
// MIGRATED does (exit 12), naming relocation, by uuid as by root, recording
// no run; a MIGRATED that is no enclosure's stops so even with one.
void without_a_confirmation_a_changed_enclosure_stops()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    const auto relocation = "confirm with `archive relocate " + fixture.archive_uuid + " <root_path>`";
    const auto by_uuid = fixture.verify_by_uuid();
    CHECK(!by_uuid && by_uuid.error().kind == va::VerifyStop::Kind::Decision &&
          by_uuid.error().identity.result == va::MatchResult::Migrated &&
          by_uuid.error().message.ends_with(relocation));
    const auto by_root = fixture.verify(fixture.archive.root());
    CHECK(!by_root && by_root.error().kind == va::VerifyStop::Kind::Decision &&
          by_root.error().message.ends_with(relocation));
    // A confirmation names enclosures, not an archive: by root, where every
    // archive is a candidate, it is not taken.
    va::VerifyRequest root_confirmed;
    root_confirmed.root = fixture.archive.root();
    root_confirmed.carrier_confirmation = by_operator(enclosure_a, enclosure_b);
    const auto by_root_confirmed = fixture.verify(root_confirmed);
    CHECK(!by_root_confirmed && by_root_confirmed.error().kind == va::VerifyStop::Kind::Decision &&
          by_root_confirmed.error().identity.result == va::MatchResult::Migrated &&
          by_root_confirmed.error().message.ends_with(relocation));
    CHECK(fixture.runs().empty() && fixture.revision() == 1);

    // Two internal devices (sample), the same filesystem on another one: a
    // clone, or another disk; only a relocation accepts it.
    Enclosed internal(std::nullopt, "sata");
    internal.move_to("SAMPLE-INTERNAL-2");
    const auto refused = internal.verify_by_uuid(by_operator(enclosure_a, "SAMPLE-INTERNAL-2"));
    CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Decision &&
          refused.error().identity.result == va::MatchResult::Migrated &&
          refused.error().message.ends_with("confirm with `archive relocate " + internal.archive_uuid + " <root_path>`"));
    CHECK(internal.runs().empty() && internal.revision() == 1);
}

// The operator's confirmation: the run records it, hash-chained; it passed
// with every entry matched, so it records revision 2 with the new enclosure
// as its identity, in the transaction of the run, as a relocation does; the
// next verification, with no confirmation, matches.
void an_operator_confirmation_records_the_new_carrier()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    const auto outcome = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
    CHECK(outcome && outcome->result == "PASSED");
    if (!outcome) {
        std::cerr << "  stopped: " << outcome.error().message << '\n';
        return;
    }
    CHECK(outcome->identity.result == va::MatchResult::Migrated);
    CHECK(outcome->carrier_confirmed && outcome->carrier_confirmed->from_revision == 1 &&
          outcome->carrier_confirmed->confirmation.how == CarrierConfirmation::How::Operator);
    CHECK(outcome->carrier_revision == 2 && outcome->revision == 2 && outcome->carrier_not_recorded.empty());
    CHECK(fixture.revision() == 2);

    const auto runs = fixture.runs();
    CHECK(runs.size() == 1);
    if (runs.size() != 1) {
        return;
    }
    const auto& run = runs.front();
    CHECK(run.revision == 2 && run.result == "PASSED" && run.mode == "FULL");
    const auto counts = counts_of(run);
    CHECK(at(counts, "/carrier_confirmed") ==
          (nlohmann::json{{"how", "operator"}, {"enclosure_recorded", enclosure_a}, {"enclosure_now", enclosure_b},
              {"from_revision", 1}}));
    CHECK(!counts.contains("chunk_hashes_added_from_revision") && !counts.contains("relocated_from_revision"));
    CHECK(fixture.chain_holds());

    // Revision 2's manifest records the enclosure seen now: its identity.
    const auto manifest = va::load_recorded_manifest(fixture.store, *fixture.repository.files,
        fixture.repository.context.repo, fixture.archive_uuid, 2);
    CHECK(manifest && manifest->header.supersedes == 1 && manifest->header.identity.devices.size() == 1 &&
          manifest->header.identity.devices.front().serial == enclosure_b);
    // Its media evidence names the confirmation (not hashed; R-96).
    const auto evidence = fixture.store.run_evidence(run.run_id);
    CHECK(evidence && *evidence && (*evidence)->match_result == "MIGRATED" &&
          (*evidence)->operator_note == "enclosure changed, confirmed by the operator without the serial");
    // Nothing read of the device: no confirmed device, nothing cached.
    const auto confirmed = va::read_confirmed_device(fixture.store, fixture.archive_uuid);
    CHECK(confirmed && !*confirmed);
    CHECK(fixture.store.setting(va::device_identity_key(fixture.archive_uuid)).value_or("x").empty());

    // The next verification needs no confirmation: MATCH.
    const auto next = fixture.verify_by_uuid();
    CHECK(next && next->result == "PASSED" && next->identity.result == va::MatchResult::Match &&
          !next->carrier_confirmed && next->revision == 2);
    CHECK(fixture.chain_holds());

    // The history says so beside the run, in both languages, and --json gives it.
    const Text english(Language::English);
    const Text chinese(Language::TraditionalChinese);
    CHECK(va::app::history_note(english, run) == "enclosure changed, confirmed by the operator without the serial");
    CHECK(va::app::history_note(chinese, run) == "外接盒已更換，由您確認、未讀序號");
    const auto json = va::app::history_run_json(run);
    CHECK(at(json, "/carrier_confirmed/how") == "operator" && at(json, "/carrier_confirmed/from_revision") == 1 &&
          json["relocation"].is_null() && json["revision"] == 2);
    const auto later = fixture.runs();
    CHECK(later.size() == 2 && va::app::history_note(english, later.back()).empty() &&
          va::app::history_run_json(later.back())["carrier_confirmed"].is_null());

    // verify's text output and --json say what the run recorded of the new
    // carrier, as the window's report does; nothing for the next run.
    CHECK(va::app::carrier_line(english, *outcome) ==
          "new enclosure recorded: revision 2 records it (confirmed by you, serial not read)");
    CHECK(va::app::carrier_line(english, *outcome, true) ==
          "New enclosure recorded: revision 2 records it (confirmed by you, serial not read)");
    CHECK(va::app::carrier_line(chinese, *outcome) == "已記下新的外接盒：由版次 2 記錄（由您確認、未讀序號）");
    CHECK(va::app::carrier_line(chinese, *outcome, true) == "已記下新的外接盒：由版次 2 記錄（由您確認、未讀序號）");
    const auto member = va::app::carrier_member(*outcome);
    CHECK(at(member, "/carrier_confirmed/how") == "operator" && at(member, "/carrier_confirmed/revision") == 2 &&
          at(member, "/carrier_confirmed/from_revision") == 1 &&
          at(member, "/carrier_confirmed/enclosure_now") == enclosure_b &&
          member["carrier_confirmed"].contains("not_recorded") && member["carrier_confirmed"]["not_recorded"].is_null());
    const nlohmann::json none{{"carrier_confirmed", nullptr}};
    CHECK(next && va::app::carrier_line(english, *next).empty() && va::app::carrier_member(*next) == none);
}

// A wrong "same device" is audited too: a confirmed run that FAILED is
// recorded with its confirmation, and records no revision and nothing of the
// carrier: the next verification stops as MIGRATED again.
void a_failed_confirmed_run_is_audited()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    fixture.archive.node("SAMPLE-notes.txt").content = "sample notez"; // a clone that differs (sample)
    const auto outcome = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
    CHECK(outcome && outcome->result == "FAILED" && outcome->carrier_confirmed && outcome->carrier_revision == 0 &&
          outcome->revision == 1 && outcome->carrier_not_recorded == "the verification did not pass");
    const auto runs = fixture.runs();
    CHECK(runs.size() == 1 && runs.front().result == "FAILED" && runs.front().revision == 1 &&
          at(counts_of(runs.front()), "/carrier_confirmed/how") == "operator");
    CHECK(fixture.revision() == 1 && fixture.chain_holds());
    const auto revisions = fixture.store.revisions(fixture.archive_uuid);
    CHECK(revisions && revisions->size() == 1);
    // The history says so beside the FAILED run too, and the report says
    // that nothing of the carrier was recorded.
    const Text english(Language::English);
    const Text chinese(Language::TraditionalChinese);
    CHECK(runs.size() == 1 &&
          va::app::history_note(english, runs.front()) ==
              "enclosure changed, confirmed by the operator without the serial");
    if (outcome) {
        CHECK(va::app::carrier_line(english, *outcome) == "new enclosure not recorded: the verification did not pass");
        CHECK(va::app::carrier_line(chinese, *outcome, true) == "未記下新的外接盒：驗證未通過");
        const auto member = va::app::carrier_member(*outcome);
        CHECK(at(member, "/carrier_confirmed/not_recorded") == "the verification did not pass" &&
              member["carrier_confirmed"]["revision"].is_null());
    }
    const auto again = fixture.verify_by_uuid();
    CHECK(!again && again.error().identity.result == va::MatchResult::Migrated);
}

// A stopped confirmed run keeps its confirmation: its INCOMPLETE row and its
// record ("run:<id>") carry it, and its resume, given none, goes on with it
// behind that enclosure only, and records the new carrier once it passes.
void a_resume_keeps_the_confirmation()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    fixture.archive.node("SAMPLE-photos/a.jpg").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
    const auto stopped = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
    CHECK(stopped && stopped->result == "INCOMPLETE" && stopped->carrier_confirmed);
    if (!stopped) {
        return;
    }
    const auto runs = fixture.runs();
    CHECK(runs.size() == 1 && runs.front().result == "INCOMPLETE" &&
          at(counts_of(runs.front()), "/carrier_confirmed/enclosure_now") == enclosure_b);
    const auto record = fixture.store.setting("run:" + stopped->run_id);
    CHECK(record && va::carrier_confirmed_in_record(nlohmann::json::parse(*record, nullptr, false)));
    CHECK(fixture.revision() == 1);

    fixture.archive.node("SAMPLE-photos/a.jpg").open_error.reset();
    va::VerifyRequest resume;
    resume.resume_run = stopped->run_id;
    // Behind the enclosure recorded before, the confirmation no longer holds.
    fixture.move_to(enclosure_a);
    const auto elsewhere = fixture.verify(resume);
    CHECK(!elsewhere && elsewhere.error().kind == va::VerifyStop::Kind::Decision &&
          elsewhere.error().message.starts_with("run " + stopped->run_id + " cannot be resumed"));
    // Nor behind a third one.
    fixture.move_to(enclosure_c);
    const auto third = fixture.verify(resume);
    CHECK(!third && third.error().message.starts_with("run " + stopped->run_id + " cannot be resumed"));
    // Behind the confirmed one, it resumes, and records revision 2.
    fixture.move_to(enclosure_b);
    const auto resumed = fixture.verify(resume);
    CHECK(resumed && resumed->result == "PASSED" && resumed->resumed_from == stopped->run_id &&
          resumed->carrier_confirmed && resumed->carrier_confirmed->from_revision == 1 &&
          resumed->carrier_revision == 2);
    const auto after = fixture.runs();
    CHECK(after.size() == 2 && after.back().revision == 2 &&
          at(counts_of(after.back()), "/carrier_confirmed/how") == "operator" &&
          counts_of(after.back())["resumed_from"] == stopped->run_id);
    CHECK(fixture.revision() == 2 && fixture.chain_holds());
}

// The device's own serial read at registration is evidence (its media
// evidence, the settings cache), and the archive's confirmed device: a later
// enclosure change with that serial goes on by it, records the new carrier,
// and so again behind a third enclosure; another serial is refused, as is a
// serial where the archive has no confirmed device.
void the_confirmed_device_goes_on_by_its_serial()
{
    Enclosed fixture(sample_read("SAMPLE-DEVICE-1"));
    // The registration's evidence holds the read; 1.1.0 reads that evidence as
    // it always did, ignoring the member.
    const auto evidence = fixture.store.archive_evidence(fixture.archive_uuid);
    CHECK(evidence && evidence->size() == 1 && evidence->front().operator_note == "registered");
    if (evidence && !evidence->empty()) {
        const auto read = va::device_read_of_evidence(evidence->front());
        CHECK(read && read->device.serial == "SAMPLE-DEVICE-1" && read->how == "sat" &&
              read->read_at == "2026-11-04T08:00:00Z" && read->device_ref == "SAMPLE-DEVICE-REF");
        const auto volume = va::volume_from_json(nlohmann::json::parse(evidence->front().volume_json));
        CHECK(volume.devices.size() == 1 && volume.devices.front().serial == enclosure_a);
    }
    const auto cached = fixture.store.setting(va::device_identity_key(fixture.archive_uuid));
    const auto cached_device =
        cached ? va::confirmed_device_from_json(nlohmann::json::parse(*cached, nullptr, false)) : std::nullopt;
    CHECK(cached_device && cached_device->device.serial == "SAMPLE-DEVICE-1" && cached_device->run_id.empty());
    auto confirmed = va::read_confirmed_device(fixture.store, fixture.archive_uuid);
    CHECK(confirmed && *confirmed && (*confirmed)->device.serial == "SAMPLE-DEVICE-1" && (*confirmed)->run_id.empty());

    fixture.move_to(enclosure_b);
    // Another device's serial is refused, recording nothing.
    const auto other = fixture.verify_by_uuid(by_serial(enclosure_a, enclosure_b, sample_device("SAMPLE-DEVICE-9", "")));
    CHECK(!other && other.error().kind == va::VerifyStop::Kind::Refused &&
          other.error().message == "the device's own serial SAMPLE-DEVICE-9 is not the serial SAMPLE-DEVICE-1 of the "
                                   "device of archive " +
                                       fixture.archive_uuid + ": it is another device");
    CHECK(fixture.runs().empty());

    const auto first = fixture.verify_by_uuid(by_serial(enclosure_a, enclosure_b, sample_device("SAMPLE-DEVICE-1")));
    CHECK(first && first->result == "PASSED" && first->carrier_revision == 2);
    const Text english(Language::English);
    const Text chinese(Language::TraditionalChinese);
    auto runs = fixture.runs();
    CHECK(runs.size() == 1 && va::app::history_note(english, runs.back()) == "enclosure changed, same device by its serial" &&
          va::app::history_note(chinese, runs.back()) == "外接盒已更換，裝置本身的序號相符");
    CHECK(runs.size() == 1 && at(counts_of(runs.back()), "/carrier_confirmed/device/serial") == "SAMPLE-DEVICE-1");
    confirmed = va::read_confirmed_device(fixture.store, fixture.archive_uuid);
    CHECK(confirmed && *confirmed && first && (*confirmed)->run_id == first->run_id);

    // The enclosure changes again: the same serial goes on by it.
    fixture.move_to(enclosure_c);
    const auto second = fixture.verify_by_uuid(by_serial(enclosure_b, enclosure_c, sample_device("SAMPLE-DEVICE-1")));
    CHECK(second && second->result == "PASSED" && second->carrier_revision == 3);
    CHECK(fixture.revision() == 3 && fixture.chain_holds());

    // An archive with no confirmed device takes no confirmation by a serial.
    Enclosed unread;
    unread.move_to(enclosure_b);
    const auto nothing = unread.verify_by_uuid(by_serial(enclosure_a, enclosure_b, sample_device("SAMPLE-DEVICE-1")));
    CHECK(!nothing && nothing.error().kind == va::VerifyStop::Kind::Refused &&
          nothing.error().message.starts_with("archive " + unread.archive_uuid + " has no confirmed device"));
    // The operator compares the serial read with the label: that read is the
    // archive's confirmed device once the run records the new carrier.
    const auto compared = unread.verify_by_uuid(by_operator(enclosure_a, enclosure_b, sample_device("SAMPLE-DEVICE-2")));
    CHECK(compared && compared->result == "PASSED" && compared->carrier_revision == 2);
    const auto unread_runs = unread.runs();
    CHECK(unread_runs.size() == 1 &&
          va::app::history_note(english, unread_runs.back()) ==
              "enclosure changed, confirmed by the operator after reading the device's serial" &&
          va::app::history_note(chinese, unread_runs.back()) == "外接盒已更換，讀取序號後由您確認");
    const auto now_confirmed = va::read_confirmed_device(unread.store, unread.archive_uuid);
    CHECK(now_confirmed && *now_confirmed && (*now_confirmed)->device.serial == "SAMPLE-DEVICE-2");
    const auto now_cached = unread.store.setting(va::device_identity_key(unread.archive_uuid));
    CHECK(now_cached && now_cached->contains("SAMPLE-DEVICE-2"));
}

// A confirmation holds only for the enclosures it names, for an archive that
// takes new revisions, and never adds chunk hashes; refused, nothing is
// recorded. Where the carrier matches as recorded, it is not used.
void a_confirmation_holds_for_its_enclosures_only()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    const auto other = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_c));
    CHECK(!other && other.error().kind == va::VerifyStop::Kind::Refused &&
          other.error().message == "the confirmation names the enclosure SAMPLE-ENCLOSURE-C (recorded: "
                                   "SAMPLE-ENCLOSURE-A), but archive " +
                                       fixture.archive_uuid +
                                       " is seen through the enclosure SAMPLE-ENCLOSURE-B (recorded: "
                                       "SAMPLE-ENCLOSURE-A); confirm it again");
    va::VerifyRequest adding;
    adding.archive_uuid = fixture.archive_uuid;
    adding.carrier_confirmation = by_operator(enclosure_a, enclosure_b);
    adding.add_chunk_hashes = true;
    adding.chunk_size = 64 * 1024;
    const auto chunks = fixture.verify(adding);
    CHECK(!chunks && chunks.error().kind == va::VerifyStop::Kind::Refused &&
          chunks.error().message.starts_with("chunk hashes are not added by the verification that accepts another "
                                             "enclosure"));
    CHECK(fixture.runs().empty() && fixture.revision() == 1);

    // A retired archive takes no new revisions (decision 0003).
    auto record = fixture.store.find_archive(fixture.archive_uuid);
    CHECK(record && *record);
    if (record && *record) {
        auto retired = **record;
        retired.status = "RETIRED";
        CHECK(fixture.store.update_archive(retired).has_value());
        const auto refused = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
        CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused &&
              refused.error().message == "a retired archive takes no new revisions");
        CHECK(fixture.runs().empty());
    }

    // Behind the enclosure it records, the archive is verified as always.
    Enclosed matching;
    const auto outcome = matching.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
    CHECK(outcome && outcome->result == "PASSED" && outcome->identity.result == va::MatchResult::Match &&
          !outcome->carrier_confirmed && outcome->revision == 1);
    const auto runs = matching.runs();
    CHECK(runs.size() == 1 && !counts_of(runs.front()).contains("carrier_confirmed"));
}

// One operation that creates the archive's next revision at a time (decision
// 0033): a confirmed verification, which records revision N+1, does not begin
// while a relocation of the archive is unfinished (here one interrupted
// behind the new enclosure, sample), and leaves its record and saved progress
// as they are; refused so, it names the relocation for the window's words.
void a_confirmation_waits_for_another_operation()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    fixture.archive.node("SAMPLE-photos/a.jpg").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
    const auto interrupted = fixture.relocate_here();
    fixture.archive.node("SAMPLE-photos/a.jpg").open_error.reset();
    CHECK(!interrupted && interrupted.error().kind == va::RelocateFailure::Kind::Incomplete);
    const auto record_before = fixture.store.setting(va::relocation_key(fixture.archive_uuid)).value_or("");
    CHECK(!record_before.empty());
    const auto runs_before = fixture.runs().size();

    const auto refused = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
    CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused && refused.error().refusing &&
          refused.error().refusing->kind == va::RefusingOperation::Kind::Relocation &&
          refused.error().identity.archives.empty() &&
          refused.error().message.starts_with("an unfinished relocation of archive " + fixture.archive_uuid));
    CHECK(fixture.runs().size() == runs_before && fixture.revision() == 1);
    CHECK(fixture.store.setting(va::relocation_key(fixture.archive_uuid)).value_or("") == record_before);
    // Without a confirmation it stops as every MIGRATED does, as before.
    const auto plain = fixture.verify_by_uuid();
    CHECK(!plain && plain.error().kind == va::VerifyStop::Kind::Decision &&
          plain.error().identity.result == va::MatchResult::Migrated);
}

// A sector-by-sector clone of the archive's device, registered as another
// archive (a copy of it) behind the enclosure seen now (sample twin): that
// volume is the twin's carrier exactly as recorded, so the confirmation of the
// first archive is refused there, recording nothing (decision 0001: two
// archives never record one volume at overlapping roots). Seen through a
// third enclosure, it is MIGRATED for both twins: that is the operator's
// question, and the confirmation goes on.
void a_twin_carrier_is_not_taken()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    va::RegisterRequest twin_request;
    twin_request.root = fixture.archive.root();
    twin_request.force_new_archive = true;
    twin_request.copy_of = fixture.archive_uuid;
    const auto twin = fixture.register_root(twin_request);
    CHECK(twin && twin->archive_uuid != fixture.archive_uuid);
    if (!twin) {
        std::cerr << "  twin not registered: " << twin.error().message << '\n';
        return;
    }
    const auto refused = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
    CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused &&
          refused.error().message == "this volume is the carrier of archive " + twin->archive_uuid +
                                         " as recorded, so it is not taken for the device of archive " +
                                         fixture.archive_uuid + "; verify archive " + twin->archive_uuid +
                                         ", or attach the device of archive " + fixture.archive_uuid +
                                         " through another enclosure");
    CHECK(fixture.runs().empty() && fixture.revision() == 1);
    // The twin verifies as recorded.
    va::VerifyRequest twin_verify;
    twin_verify.archive_uuid = twin->archive_uuid;
    const auto twin_verified = fixture.verify(twin_verify);
    CHECK(twin_verified && twin_verified->result == "PASSED" &&
          twin_verified->identity.result == va::MatchResult::Match);

    fixture.move_to(enclosure_c);
    const auto third = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_c));
    CHECK(third && third->result == "PASSED" && third->carrier_revision == 2);
    CHECK(fixture.revision() == 2 && fixture.chain_holds());
}

// The device read that a registration began with goes on with its resume,
// which asks for no administrator permission again: the plan keeps it, and the
// registration that finishes records it in its media evidence and the cache.
void a_resumed_registration_keeps_the_device_read()
{
    test::EngineFixture fixture;
    auto& device = fixture.media.volume.devices.front();
    device.model = "SAMPLE USB Enclosure";
    device.serial = enclosure_a;
    device.bus = "usb";
    fixture.archive.add_file("SAMPLE-a.txt", "sample a");
    fixture.archive.add_file("SAMPLE-b.txt", "sample b");
    fixture.archive.node("SAMPLE-b.txt").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
    va::RegisterRequest request;
    request.root = fixture.archive.root();
    request.device_read = sample_read("SAMPLE-DEVICE-1");
    const auto interrupted = fixture.register_root(request);
    fixture.archive.node("SAMPLE-b.txt").open_error.reset();
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->size() == 1);
    if (!archives || archives->size() != 1) {
        return;
    }
    const auto archive_uuid = archives->front().archive_uuid;
    const auto plan = nlohmann::json::parse(fixture.store.setting("registration:" + archive_uuid).value_or(""),
        nullptr, false);
    CHECK(at(plan, "/device_identity/serial") == "SAMPLE-DEVICE-1" && at(plan, "/device_identity/how") == "sat");
    CHECK(fixture.store.setting(va::device_identity_key(archive_uuid)).value_or("x").empty());

    va::RegisterRequest resume;
    resume.root = fixture.archive.root();
    resume.resume = true;
    const auto resumed = fixture.register_root(resume);
    CHECK(resumed && resumed->archive_uuid == archive_uuid && resumed->revision == 1);
    const auto evidence = fixture.store.archive_evidence(archive_uuid);
    CHECK(evidence && !evidence->empty());
    if (evidence && !evidence->empty()) {
        const auto& registered = evidence->back();
        const auto read = va::device_read_of_evidence(registered);
        CHECK(registered.operator_note == "registered" && read && read->device.serial == "SAMPLE-DEVICE-1");
    }
    const auto cached = fixture.store.setting(va::device_identity_key(archive_uuid));
    const auto cached_device =
        cached ? va::confirmed_device_from_json(nlohmann::json::parse(*cached, nullptr, false)) : std::nullopt;
    CHECK(cached_device && cached_device->device.serial == "SAMPLE-DEVICE-1");
    const auto confirmed = va::read_confirmed_device(fixture.store, archive_uuid);
    CHECK(confirmed && *confirmed && (*confirmed)->device.serial == "SAMPLE-DEVICE-1");
}

// A confirmed run that passed but cannot record the new carrier says why, and
// leaves the archive at revision 1 with its confirmed device as it was: a
// name that cannot exist on this system (it would drop out of revision 2);
// another operation that recorded a revision meanwhile; the archive retired
// meanwhile. The next verification stops as MIGRATED again.
void a_pass_that_cannot_record_the_carrier()
{
    {
        // Registered on Linux with a name Windows cannot hold (sample).
        Enclosed fixture(sample_read("SAMPLE-DEVICE-1"), "usb", [](Enclosed& before) {
            before.host.os = "linux";
            before.archive.add_file("SAMPLE-what?.txt", "a name Linux allows (sample)");
        });
        fixture.host.os = "windows";
        fixture.archive.remove("SAMPLE-what?.txt");
        fixture.move_to(enclosure_b);
        const auto outcome =
            fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b, sample_device("SAMPLE-DEVICE-2")));
        CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && outcome->carrier_confirmed &&
              outcome->carrier_revision == 0 && outcome->revision == 1 &&
              outcome->carrier_not_recorded ==
                  "some names cannot exist on this system and would drop out of a new revision (verify the archive "
                  "on a system where they can exist)");
        const auto runs = fixture.runs();
        CHECK(runs.size() == 1 && runs.front().revision == 1 &&
              at(counts_of(runs.front()), "/carrier_confirmed/device/serial") == "SAMPLE-DEVICE-2");
        CHECK(fixture.revision() == 1 && fixture.chain_holds());
        const auto confirmed = va::read_confirmed_device(fixture.store, fixture.archive_uuid);
        CHECK(confirmed && *confirmed && (*confirmed)->device.serial == "SAMPLE-DEVICE-1");
        CHECK(fixture.store.setting(va::device_identity_key(fixture.archive_uuid)).value_or("").contains(
            "SAMPLE-DEVICE-1"));
        const auto again = fixture.verify_by_uuid();
        CHECK(!again && again.error().identity.result == va::MatchResult::Migrated);
    }
    // Another operation recorded a revision, or retired the archive, while
    // the run read: the record finds it in its transaction (as decision 0033's
    // record does), after revision 2's manifest was written, never named.
    const auto meanwhile = [](const std::string& change, const std::string& why_of) {
        Enclosed fixture;
        fixture.move_to(enclosure_b);
        const auto sql = change + " WHERE archive_uuid = '" + fixture.archive_uuid + "'";
        test::StagingHookWriter writer(*fixture.repository.writer, [&] { fixture.repository.execute(sql.c_str()); });
        const auto outcome = fixture.verify_by_uuid_through(writer, by_operator(enclosure_a, enclosure_b));
        CHECK(outcome && outcome->result == "PASSED" && outcome->carrier_revision == 0 && outcome->revision == 1 &&
              outcome->carrier_not_recorded ==
                  (why_of == "revision" ? "archive " + fixture.archive_uuid +
                                              " is at revision 2 now, recorded by another operation while this "
                                              "verification ran"
                                        : "archive " + fixture.archive_uuid +
                                              " was retired while this verification ran; a retired archive takes no "
                                              "new revisions"));
        if (!outcome) {
            std::cerr << "  stopped: " << outcome.error().message << '\n';
        }
        const auto revisions = fixture.store.revisions(fixture.archive_uuid);
        CHECK(revisions && revisions->size() == 1);
        const auto runs = fixture.runs();
        CHECK(runs.size() == 1 && runs.front().revision == 1 && runs.front().result == "PASSED" &&
              at(counts_of(runs.front()), "/carrier_confirmed/how") == "operator");
        CHECK(!std::filesystem::exists(va::manifest_path(
            fixture.repository.context.repo.manifests(), fixture.archive_uuid, 2)));
    };
    meanwhile("UPDATE archives SET current_revision = 2", "revision");
    meanwhile("UPDATE archives SET status = 'RETIRED'", "retired");
}

// A confirmed run that passes with warnings, every entry matched as a
// relocation requires (a changed modification time, sample), records revision
// 2 with the metadata seen now; the next verification matches without them.
void a_pass_with_warnings_records_the_carrier()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    constexpr std::int64_t touched = 1'700'000'100'000'000'000;  // a later modification time (sample)
    fixture.archive.node("SAMPLE-notes.txt").mtime_ns = touched;
    const auto outcome = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && outcome->carrier_revision == 2 &&
          outcome->carrier_not_recorded.empty());
    CHECK(outcome && std::ranges::any_of(outcome->items, [](const va::VerifyItemRecord& item) {
        return item.status == "MetadataMismatch" && item.path.p == "SAMPLE-notes.txt";
    }));
    CHECK(fixture.revision() == 2 && fixture.chain_holds());
    const auto manifest = va::load_recorded_manifest(fixture.store, *fixture.repository.files,
        fixture.repository.context.repo, fixture.archive_uuid, 2);
    CHECK(manifest && std::ranges::any_of(manifest->entries, [&](const va::Entry& entry) {
        return entry.path.p == "SAMPLE-notes.txt" && entry.mtime_ns == touched;
    }));
    const auto next = fixture.verify_by_uuid();
    CHECK(next && next->result == "PASSED" && next->identity.result == va::MatchResult::Match);
}

// No schema change (1.1.0 still opens the repository): schema version 3,
// verify_runs with its 17 columns, the confirmation inside counts_json alone,
// which the chain hashes as stored, so `repo verify-history` holds; and a
// changed counts_json breaks it, as any edited run does.
void the_records_are_those_of_the_schema()
{
    Enclosed fixture;
    fixture.move_to(enclosure_b);
    const auto outcome = fixture.verify_by_uuid(by_operator(enclosure_a, enclosure_b));
    CHECK(outcome && outcome->carrier_revision == 2);
    CHECK(fixture.repository.query("SELECT version FROM schema_version") == "3");
    CHECK(fixture.repository.query("SELECT count(*) FROM pragma_table_info('verify_runs')") == "17");
    CHECK(fixture.chain_holds());
    fixture.repository.execute(
        "UPDATE verify_runs SET counts_json = replace(counts_json, '\"operator\"', '\"device_serial\"')");
    const auto checked = fixture.store.check_history(fixture.repository.context.repository_uuid);
    CHECK(checked && checked->first_broken_run);
}

// confirmed_device from made-up runs and evidence: a registration's read; a
// run that recorded the new carrier with a read; neither a FAILED run, nor a
// run that recorded nothing, nor an operator confirmation without a read
// changes it; a relocation ends it.
void the_confirmed_device_follows_the_history()
{
    const auto registration = [](const std::string& serial) {
        va::VolumeInfo volume = test::fake_volume("/vol");
        return va::EvidenceRecord{"SAMPLE-ARCHIVE", "", "2026-11-01T00:00:00Z",
            va::volume_json_with_device_read(volume, sample_read(serial)), "UNKNOWN", "registered"};
    };
    const auto run_with = [](const std::string& id, std::uint32_t revision, const std::string& result,
                              const CarrierConfirmation& confirmation, std::uint32_t from) {
        va::VerifyRunRecord run;
        run.run_id = id;
        run.revision = revision;
        run.result = result;
        run.finished = "2026-11-02T00:00:00Z";
        run.counts_json = nlohmann::json{{"items", nlohmann::json::object()},
            {"carrier_confirmed", va::carrier_confirmation_json(confirmation, from)}}
                              .dump();
        return run;
    };
    const auto seen = [](const std::string& run_id) {
        return va::EvidenceRecord{"SAMPLE-ARCHIVE", run_id, "2026-11-02T00:00:00Z", "{}", "MIGRATED", ""};
    };
    std::vector<va::VerifyRunRecord> runs;
    std::vector<va::EvidenceRecord> evidence;
    CHECK(!va::confirmed_device(runs, evidence));
    evidence.push_back(registration("SAMPLE-DEVICE-1"));
    auto device = va::confirmed_device(runs, evidence);
    CHECK(device && device->device.serial == "SAMPLE-DEVICE-1" && device->run_id.empty() &&
          device->confirmed_at == "2026-11-01T00:00:00Z");

    // FAILED, and passed without recording the carrier: nothing changes.
    runs.push_back(run_with("run-failed", 1, "FAILED",
        by_operator(enclosure_a, enclosure_b, sample_device("SAMPLE-DEVICE-7")), 1));
    evidence.push_back(seen("run-failed"));
    runs.push_back(run_with("run-unrecorded", 1, "PASSED",
        by_operator(enclosure_a, enclosure_b, sample_device("SAMPLE-DEVICE-8")), 1));
    evidence.push_back(seen("run-unrecorded"));
    device = va::confirmed_device(runs, evidence);
    CHECK(device && device->device.serial == "SAMPLE-DEVICE-1");
    // An operator confirmation without a read keeps the device it had.
    runs.push_back(run_with("run-operator", 2, "PASSED", by_operator(enclosure_a, enclosure_b), 1));
    evidence.push_back(seen("run-operator"));
    device = va::confirmed_device(runs, evidence);
    CHECK(device && device->device.serial == "SAMPLE-DEVICE-1");
    // A run that recorded the new carrier with a read gives its device.
    runs.push_back(run_with("run-read", 3, "PASSED_WITH_WARNINGS",
        by_operator(enclosure_b, enclosure_c, sample_device("SAMPLE-DEVICE-2")), 2));
    evidence.push_back(seen("run-read"));
    device = va::confirmed_device(runs, evidence);
    CHECK(device && device->device.serial == "SAMPLE-DEVICE-2" && device->run_id == "run-read");
    // A relocation ends it; a registration's read after it gives one again.
    va::VerifyRunRecord relocated;
    relocated.run_id = "run-relocated";
    relocated.revision = 4;
    relocated.result = "PASSED";
    relocated.counts_json = R"({"items":{},"relocated_from_revision":3})";
    runs.push_back(relocated);
    evidence.push_back(seen("run-relocated"));
    CHECK(!va::confirmed_device(runs, evidence));
    evidence.push_back(registration("SAMPLE-DEVICE-3"));
    device = va::confirmed_device(runs, evidence);
    CHECK(device && device->device.serial == "SAMPLE-DEVICE-3");
    // Evidence of no run and no registration (a verification that stopped) is none.
    evidence.push_back(va::EvidenceRecord{"SAMPLE-ARCHIVE", "", "2026-11-03T00:00:00Z",
        va::volume_json_with_device_read(test::fake_volume("/vol"), sample_read("SAMPLE-DEVICE-4")), "MIGRATED", ""});
    device = va::confirmed_device(runs, evidence);
    CHECK(device && device->device.serial == "SAMPLE-DEVICE-3");

    // The cache's value, both ways.
    const auto json = va::confirmed_device_json(*device);
    const auto back = va::confirmed_device_from_json(json);
    CHECK(back && back->device.serial == "SAMPLE-DEVICE-3" && back->confirmed_at == device->confirmed_at);
    CHECK(!va::confirmed_device_from_json(nlohmann::json{{"model", "SAMPLE"}}));
}

// A relocation puts the archive on a carrier whose device nobody confirmed:
// the cached confirmed device goes, and confirmed_device ends there.
void a_relocation_ends_the_confirmed_device()
{
    Enclosed fixture(sample_read("SAMPLE-DEVICE-1"), "sata");
    CHECK(!fixture.store.setting(va::device_identity_key(fixture.archive_uuid)).value_or("").empty());
    fixture.move_to("SAMPLE-INTERNAL-2");
    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
        fixture.host, fixture.processes};
    va::RelocateRequest request;
    request.archive_uuid = fixture.archive_uuid;
    request.root = fixture.archive.root();
    const auto relocated = va::relocate_archive(services, request, fixture.cancel, fixture.progress);
    CHECK(relocated && relocated->revision == 2);
    CHECK(fixture.store.setting(va::device_identity_key(fixture.archive_uuid)).value_or("x").empty());
    const auto confirmed = va::read_confirmed_device(fixture.store, fixture.archive_uuid);
    CHECK(confirmed && !*confirmed);
    // The relocation wrote its revision as before, through the shared writer.
    const auto manifest = va::load_recorded_manifest(fixture.store, *fixture.repository.files,
        fixture.repository.context.repo, fixture.archive_uuid, 2);
    CHECK(manifest && manifest->header.identity.devices.front().serial == "SAMPLE-INTERNAL-2" &&
          manifest->header.supersedes == 1 && manifest->header.root_hint == va::path_to_utf8(fixture.archive.root()));
}

// The window's words and judgments (decision 0047), in both languages.
void the_window_words()
{
    const Text english(Language::English);
    const Text chinese(Language::TraditionalChinese);
    auto recorded = test::fake_volume("/vol");
    recorded.devices.front().serial = enclosure_a;
    auto now = recorded;
    now.devices.front().serial = enclosure_b;
    const va::ConfirmedDevice device{sample_device("SAMPLE-DEVICE-1"), "2026-11-01T00:00:00Z", ""};

    const auto without = va::app::enclosure_facts("ARCH-SAMPLE", recorded, now, std::nullopt);
    CHECK(without.label == "ARCH-SAMPLE" && without.enclosure_recorded == enclosure_a &&
          without.enclosure_now == enclosure_b && !without.confirmed);
    const auto with = va::app::enclosure_facts("ARCH-SAMPLE", recorded, now, device);
    CHECK(with.confirmed && with.confirmed->device.serial == "SAMPLE-DEVICE-1");

    // The read: the confirmed device's serial verifies at once; another is not
    // this archive's device; with none, the operator compares it.
    CHECK(va::app::judge_device_read(sample_device("SAMPLE-DEVICE-1", ""), device) ==
          va::app::DeviceReadVerdict::SameDevice);
    CHECK(va::app::judge_device_read(sample_device("SAMPLE-DEVICE-9", "5000c0ffee000009"), device) ==
          va::app::DeviceReadVerdict::OtherDevice);
    CHECK(va::app::judge_device_read(sample_device("SAMPLE-DEVICE-1"), std::nullopt) ==
          va::app::DeviceReadVerdict::NothingToCompare);
    const auto serial = va::app::confirmation_by_serial(with, sample_device("SAMPLE-DEVICE-1"));
    CHECK(serial.how == CarrierConfirmation::How::DeviceSerial && serial.device &&
          serial.enclosure_recorded == enclosure_a && serial.enclosure_now == enclosure_b);
    const auto unread = va::app::confirmation_by_operator(without, std::nullopt);
    CHECK(unread.how == CarrierConfirmation::How::Operator && !unread.device && unread.enclosure_now == enclosure_b);
    const auto compared = va::app::confirmation_by_operator(without, sample_device("SAMPLE-DEVICE-2"));

    CHECK(va::app::enclosure_question_title(english, "SAMPLE:") ==
          "The enclosure of SAMPLE: differs from the one recorded");
    CHECK(va::app::enclosure_question_title(chinese, "SAMPLE:") == "SAMPLE: 的外接盒與記錄中的不同");
    const auto asked = va::app::enclosure_question_body(chinese, with);
    // The two enclosures' serials are the window's to list beside it.
    CHECK(asked.size() == 2 && asked[0] == "這個磁碟區的檔案系統 ID 和 ARCH-SAMPLE 記錄中的相同，但外接盒回報的序號換了。" &&
          asked[1] == "ARCH-SAMPLE 記錄了裝置本身的序號 SAMPLE-DEVICE-1。讀取這次的序號，就能判斷是不是同一個裝置。");
    const auto unknown = va::app::enclosure_question_body(english, without);
    CHECK(unknown.size() == 3 && unknown[1].contains("sector-by-sector clone") &&
          unknown[1].contains("passes a FULL verify too") && unknown[2].contains("label"));
    CHECK(va::app::enclosure_question_body(chinese, without)[1].contains("完整驗證也會通過"));
    CHECK(va::app::enclosure_question_body(chinese, without)[1] == va::app::sector_clone_words(chinese));
    // Named as Windows names an unsigned program, never in its words "to
    // make changes to your device", whose device is the computer.
    const auto note = va::app::device_identity_reader_note(chinese);
    CHECK(note.starts_with("需要系統管理員權限") && note.contains("使用者帳戶控制") &&
          note.contains("veriarchive-device-identity.exe") && note.contains("不明發行者") && !note.contains("變更"));
    CHECK(!va::app::device_identity_reader_note(english).contains("make changes"));
    CHECK(va::app::device_read_declined(chinese).contains("不讀序號，自行確認"));
    // Another enclosure, never the device attached directly: taken out of its
    // enclosure, it is no changed enclosure (only a relocation accepts it).
    CHECK(va::app::device_read_failed(english).contains("ATA commands") &&
          va::app::device_read_failed(english).contains("Try another enclosure") &&
          !va::app::device_read_failed(english).contains("SATA"));
    CHECK(va::app::device_read_failed(chinese).contains("換一個外接盒") &&
          !va::app::device_read_failed(chinese).contains("SATA"));

    // The operator confirmation says that a clone passes too, and that the
    // history records it whatever the result; only a pass records the new
    // enclosure.
    CHECK(va::app::operator_confirmation_title(chinese) == "不讀序號，自行確認");
    const auto confirmation = va::app::operator_confirmation_body(english, without);
    CHECK(confirmation.size() == 2 && confirmation[0].contains("ARCH-SAMPLE") &&
          confirmation[1].contains("Whatever the result, the history records that you confirmed it") &&
          confirmation[1].contains("only when the verification passes is the new enclosure recorded") &&
          confirmation[1].contains("sector-by-sector clone passes the verification too"));
    const auto chinese_confirmation = va::app::operator_confirmation_body(chinese, without);
    CHECK(chinese_confirmation.size() == 2 &&
          chinese_confirmation[1].contains("無論結果如何，歷程都會記錄這次是由您確認") &&
          chinese_confirmation[1].contains("驗證通過之後，才會記下新的外接盒") &&
          chinese_confirmation[1].contains("複本也會通過驗證"));

    CHECK(va::app::device_read_title(english) == "What the device reports");
    CHECK(va::app::device_read_title(chinese) == "讀到的裝置");
    CHECK(va::app::device_read_body(chinese, without)[0].contains("請對照裝置外殼標籤上的序號"));
    // So does the answer after a read of the serial.
    CHECK(va::app::device_read_body(english, without)[1].contains("Whatever the result, the history records") &&
          va::app::device_read_body(chinese, without)[1].contains("無論結果如何，歷程都會記錄這次是讀取序號後由您確認"));
    CHECK(va::app::other_device_title(chinese, with) == "這不是 ARCH-SAMPLE 的裝置");
    const auto other = va::app::other_device_body(chinese, with);
    CHECK(other.size() == 2 && other[0].contains("ARCH-SAMPLE 的副本") && other[1] == "沒有記錄任何內容。");
    CHECK(va::app::same_device_started(chinese, sample_device("SAMPLE-DEVICE-1")) ==
          "裝置本身的序號相符（SAMPLE-DEVICE-1）。完整驗證通過後會記下新的外接盒。");
    // A read that matched by its WWN alone names the WWN.
    CHECK(va::app::same_device_started(english, sample_device("", "5000c0ffee000001")) ==
          "The device's own serial matches (5000c0ffee000001). Once the FULL verify passes, the new enclosure is "
          "recorded.");

    CHECK(va::app::confirmation_words(chinese, serial) == "裝置本身的序號相符");
    CHECK(va::app::confirmation_words(chinese, compared) == "讀取序號後由您確認");
    CHECK(va::app::confirmation_words(chinese, unread) == "由您確認、未讀序號");
    CHECK(va::app::confirmed_identity_words(chinese, unread) == "已遷移：外接盒與記錄中的不同（由您確認、未讀序號）");
    CHECK(va::app::confirmed_identity_words(english, serial) ==
          "MIGRATED: the enclosure differs from the one recorded (the device's serial matches)");
    // Every word of it is in the catalog: nothing shows in English in Chinese.
    for (const auto& line : va::app::enclosure_question_body(chinese, without)) {
        CHECK(!line.contains("enclosure") && !line.contains("serial"));
    }
    for (const auto& line : va::app::device_read_body(chinese, without)) {
        CHECK(!line.contains("serial"));
    }
}

// The engine's sentences of decision 0047 read in Chinese too.
void the_engine_sentences_are_translated()
{
    const Text chinese(Language::TraditionalChinese);
    const std::string uuid = "c0ffee00-0000-4000-8000-000000000047";
    const std::string twin = "c0ffee00-0000-4000-8000-000000000048";
    CHECK(chinese("the device's own serial SAMPLE-DEVICE-9 is not the serial SAMPLE-DEVICE-1 of the device of "
                  "archive " +
                  uuid + ": it is another device") ==
          "裝置本身的序號 SAMPLE-DEVICE-9 與封存 " + uuid + " 的裝置序號 SAMPLE-DEVICE-1 不同：這是另一個裝置");
    CHECK(chinese("a confirmation by the device's serial names no device") == "依裝置序號確認時，沒有提供讀到的裝置");
    CHECK(chinese("archive " + uuid + " has no confirmed device to compare the serial SAMPLE-DEVICE-1 with; only the "
                  "operator can confirm it") ==
          "封存 " + uuid + " 沒有已確認的裝置可與序號 SAMPLE-DEVICE-1 比對；只能由您確認");
    CHECK(chinese("this volume is the carrier of archive " + twin +
                  " as recorded, so it is not taken for the device of archive " + uuid + "; verify archive " + twin +
                  ", or attach the device of archive " + uuid + " through another enclosure") ==
          "這個磁碟區是封存 " + twin + " 記錄中的載體，所以不會當成封存 " + uuid + " 的裝置；請驗證封存 " + twin +
              "，或把封存 " + uuid + " 的裝置接在另一個外接盒上");

    // Why a confirmed run records no new carrier, as verify's text output
    // and the window's report give it.
    va::VerifyOutcome outcome;
    outcome.carrier_confirmed = va::RecordedConfirmation{by_operator(enclosure_a, enclosure_b), 1};
    outcome.carrier_not_recorded =
        "archive " + uuid + " was retired while this verification ran; a retired archive takes no new revisions";
    CHECK(va::app::carrier_line(chinese, outcome) ==
          "未記下新的外接盒：封存 " + uuid + " 在這次驗證期間被停用；已停用的封存不再接受新的版次");
    outcome.carrier_not_recorded = "some names cannot exist on this system and would drop out of a new revision "
                                   "(verify the archive on a system where they can exist)";
    CHECK(va::app::carrier_line(chinese, outcome, true).starts_with("未記下新的外接盒：有些名稱"));
    CHECK(va::app::carrier_line(Text(Language::English), outcome, true) ==
          "New enclosure not recorded: some names cannot exist on this system and would drop out of a new revision "
          "(verify the archive on a system where they can exist)");
    // Not finished (INCOMPLETE): nothing said yet.
    outcome.carrier_not_recorded.clear();
    CHECK(va::app::carrier_line(chinese, outcome).empty());
}

} // namespace

void carrier_confirmation_tests()
{
    enclosures_and_records();
    without_a_confirmation_a_changed_enclosure_stops();
    an_operator_confirmation_records_the_new_carrier();
    a_failed_confirmed_run_is_audited();
    a_resume_keeps_the_confirmation();
    the_confirmed_device_goes_on_by_its_serial();
    a_confirmation_holds_for_its_enclosures_only();
    a_confirmation_waits_for_another_operation();
    a_twin_carrier_is_not_taken();
    a_resumed_registration_keeps_the_device_read();
    a_pass_that_cannot_record_the_carrier();
    a_pass_with_warnings_records_the_carrier();
    the_records_are_those_of_the_schema();
    the_confirmed_device_follows_the_history();
    a_relocation_ends_the_confirmed_device();
    the_window_words();
    the_engine_sentences_are_translated();
}
