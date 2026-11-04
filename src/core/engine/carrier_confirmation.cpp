#include "core/engine/carrier_confirmation.h"

#include "core/engine/relocate_engine.h"
#include "core/identity/matcher.h"
#include "core/identity/volume_json.h"

#include <algorithm>
#include <map>

namespace va {
namespace {

std::string text_member(const nlohmann::json& object, const char* name)
{
    const auto found = object.find(name);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

nlohmann::json device_identity_json(const DeviceIdentity& device)
{
    return {{"model", device.model}, {"serial", device.serial}, {"wwn", device.wwn}};
}

// A device read with neither a serial nor a WWN names no device.
std::optional<DeviceIdentity> device_identity_from_json(const nlohmann::json& json)
{
    if (!json.is_object()) {
        return std::nullopt;
    }
    DeviceIdentity device{text_member(json, "model"), text_member(json, "serial"), text_member(json, "wwn")};
    if (device.serial.empty() && device.wwn.empty()) {
        return std::nullopt;
    }
    return device;
}

std::string lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    });
    return text;
}

// A run whose confirmation recorded the new carrier: it passed, and recorded
// the revision after the one the confirmation was given against.
bool recorded_the_carrier(const VerifyRunRecord& run, const RecordedConfirmation& confirmed)
{
    return (run.result == "PASSED" || run.result == "PASSED_WITH_WARNINGS") &&
           run.revision == confirmed.from_revision + 1;
}

} // namespace

std::string_view confirmation_how_code(CarrierConfirmation::How how)
{
    return how == CarrierConfirmation::How::DeviceSerial ? "device_serial" : "operator";
}

nlohmann::json carrier_confirmation_json(const CarrierConfirmation& confirmation, std::uint32_t from_revision)
{
    nlohmann::json json{{"how", confirmation_how_code(confirmation.how)}};
    if (confirmation.device) {
        json["device"] = device_identity_json(*confirmation.device);
    }
    json["enclosure_recorded"] = confirmation.enclosure_recorded;
    json["enclosure_now"] = confirmation.enclosure_now;
    json["from_revision"] = from_revision;
    return json;
}

std::optional<RecordedConfirmation> carrier_confirmation_from_json(const nlohmann::json& json)
{
    if (!json.is_object()) {
        return std::nullopt;
    }
    RecordedConfirmation recorded;
    const auto how = text_member(json, "how");
    if (how == confirmation_how_code(CarrierConfirmation::How::DeviceSerial)) {
        recorded.confirmation.how = CarrierConfirmation::How::DeviceSerial;
    } else if (how == confirmation_how_code(CarrierConfirmation::How::Operator)) {
        recorded.confirmation.how = CarrierConfirmation::How::Operator;
    } else {
        return std::nullopt;
    }
    const auto from = json.find("from_revision");
    if (from == json.end() || !from->is_number_unsigned()) {
        return std::nullopt;
    }
    recorded.from_revision = from->get<std::uint32_t>();
    if (const auto device = json.find("device"); device != json.end()) {
        recorded.confirmation.device = device_identity_from_json(*device);
    }
    if (recorded.confirmation.how == CarrierConfirmation::How::DeviceSerial && !recorded.confirmation.device) {
        return std::nullopt;
    }
    recorded.confirmation.enclosure_recorded = text_member(json, "enclosure_recorded");
    recorded.confirmation.enclosure_now = text_member(json, "enclosure_now");
    return recorded;
}

std::optional<RecordedConfirmation> carrier_confirmed_in_record(const nlohmann::json& record)
{
    if (!record.is_object()) {
        return std::nullopt;
    }
    const auto found = record.find(carrier_confirmed_member);
    return found == record.end() ? std::nullopt : carrier_confirmation_from_json(*found);
}

std::optional<RecordedConfirmation> carrier_confirmed(const VerifyRunRecord& run)
{
    return carrier_confirmed_in_record(nlohmann::json::parse(run.counts_json, nullptr, false));
}

nlohmann::json device_identity_read_json(const DeviceIdentityRead& read)
{
    auto json = device_identity_json(read.device);
    json["how"] = read.how;
    json["read_at"] = read.read_at;
    json["device_ref"] = read.device_ref;
    return json;
}

std::optional<DeviceIdentityRead> device_identity_read_from_json(const nlohmann::json& json)
{
    auto device = device_identity_from_json(json);
    if (!device) {
        return std::nullopt;
    }
    return DeviceIdentityRead{
        std::move(*device), text_member(json, "how"), text_member(json, "read_at"), text_member(json, "device_ref")};
}

std::string volume_json_with_device_read(const VolumeInfo& volume, const DeviceIdentityRead& read)
{
    auto json = volume_to_json(volume);
    json[device_identity_member] = device_identity_read_json(read);
    return json.dump();
}

std::optional<DeviceIdentityRead> device_read_of_evidence(const EvidenceRecord& evidence)
{
    const auto json = nlohmann::json::parse(evidence.volume_json, nullptr, false);
    if (!json.is_object()) {
        return std::nullopt;
    }
    const auto found = json.find(device_identity_member);
    return found == json.end() ? std::nullopt : device_identity_read_from_json(*found);
}

bool same_device_identity(const DeviceIdentity& left, const DeviceIdentity& right)
{
    if (serials_equal(left.serial, right.serial)) {
        return true;
    }
    return !left.wwn.empty() && lower(left.wwn) == lower(right.wwn);
}

std::string enclosure_serial(const VolumeInfo& volume)
{
    if (volume.devices.size() != 1) {
        return {};
    }
    const auto& device = volume.devices.front();
    return device.serial.empty() ? device.wwn : device.serial;
}

bool enclosure_changed(const VolumeInfo& recorded, const VolumeInfo& now)
{
    if (recorded.devices.size() != 1 || now.devices.size() != 1) {
        return false;
    }
    const auto& before = recorded.devices.front();
    const auto& seen = now.devices.front();
    return (lower(before.bus) == "usb" && lower(seen.bus) == "usb") || seen.enclosure_hides_identity;
}

std::optional<ConfirmedDevice> confirmed_device(
    std::span<const VerifyRunRecord> runs, std::span<const EvidenceRecord> evidence)
{
    // The order in which the evidence was recorded is the order of what
    // happened to the carrier: each recorded run has its evidence, written in
    // the transaction that records the run (a relocation's too), and each
    // registration its own, with no run. Timestamps, in whole seconds, could
    // not tell runs of one second apart.
    std::map<std::string, const VerifyRunRecord*> by_id;
    for (const auto& run : runs) {
        by_id[run.run_id] = &run;
    }
    std::optional<ConfirmedDevice> confirmed;
    for (const auto& seen : evidence) {
        if (seen.run_id.empty()) {
            if (!seen.operator_note.starts_with("registered")) {
                continue;
            }
            if (auto read = device_read_of_evidence(seen)) {
                confirmed = ConfirmedDevice{std::move(read->device), seen.seen_at, {}};
            }
            continue;
        }
        const auto found = by_id.find(seen.run_id);
        if (found == by_id.end()) {
            continue;
        }
        const auto& run = *found->second;
        // A relocation recorded the archive on another carrier: the device
        // confirmed before is no longer known to be the carrier's.
        if (const auto part = relocation_part(run); part && !part->interrupted_attempt) {
            confirmed.reset();
            continue;
        }
        const auto confirmation = carrier_confirmed(run);
        if (confirmation && confirmation->confirmation.device && recorded_the_carrier(run, *confirmation)) {
            confirmed = ConfirmedDevice{*confirmation->confirmation.device, run.finished, run.run_id};
        }
    }
    return confirmed;
}

nlohmann::json confirmed_device_json(const ConfirmedDevice& device)
{
    auto json = device_identity_json(device.device);
    json["confirmed_at"] = device.confirmed_at;
    json["run_id"] = device.run_id;
    return json;
}

std::optional<ConfirmedDevice> confirmed_device_from_json(const nlohmann::json& json)
{
    auto device = device_identity_from_json(json);
    if (!device) {
        return std::nullopt;
    }
    return ConfirmedDevice{std::move(*device), text_member(json, "confirmed_at"), text_member(json, "run_id")};
}

std::expected<std::optional<ConfirmedDevice>, IoError> read_confirmed_device(
    ArchiveStore& store, const Uuid& archive_uuid)
{
    const auto runs = store.verify_runs(archive_uuid);
    if (!runs) {
        return std::unexpected(runs.error());
    }
    const auto evidence = store.archive_evidence(archive_uuid);
    if (!evidence) {
        return std::unexpected(evidence.error());
    }
    return confirmed_device(*runs, *evidence);
}

} // namespace va
