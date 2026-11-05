#pragma once
// The device behind an enclosure (decision 0047). An enclosure (a USB bridge
// or dock between a device and the computer) often reports its own serial in
// place of the device's, so a device put into another enclosure, or attached
// through another dock, is seen with another serial while its filesystem is
// the same: identity matching gives MIGRATED (§B10), as it gives for a
// sector-by-sector clone. A FULL verification of the archive by its uuid goes
// on past that only with a carrier confirmation: the device's own serial, read
// with administrator permission by the device identity reader, is the one the
// archive's history confirmed before (how = device_serial), or the operator
// says that it is the same device (operator confirmation, how = operator),
// after reading the device's serial or without it. The run records the
// confirmation in its counts (carrier_confirmed), hash-chained with the run,
// whatever its result, so that a wrong "same device" is audited too; only a
// run that passed with every entry matched records the new carrier, as
// revision N+1, as a relocation does (decision 0004). No schema change, no
// manifest key: the device's own identity stays out of the manifest.
//
// A device's own identity read at registration is evidence, not history: it
// is kept in the registration's media evidence (device_identity_member), and
// the settings cache the archive's confirmed device (device_identity_key).
#include "common/types.h"
#include "core/repository/archive_store.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace va {

// DeviceIdentity and DeviceIdentityRead (common/types.h) are what the device
// identity reader (IDeviceIdentityHelper, core/interfaces.h) reads.

// What confirms that a carrier whose enclosure changed holds the archive's
// device. `device_serial`: the device's own serial was read and is the
// archive's confirmed device (confirmed_device), so the program judged it;
// `operator`: the operator said so (operator confirmation), after reading the
// device's serial (`device` then holds the read, which the operator compared
// with the serial on the device's label) or without reading it. The
// enclosure's serial as the latest revision records it and as it is seen now
// (enclosure_serial), as the operator was shown them: the confirmation holds
// for that change only.
struct CarrierConfirmation {
    enum class How { DeviceSerial, Operator };
    How how = How::Operator;
    std::optional<DeviceIdentity> device;  // the read: always for DeviceSerial
    std::string enclosure_recorded;
    std::string enclosure_now;
};

// A confirmation as a run records it: with the revision N of the archive it
// was given against, whose next revision records the new carrier.
struct RecordedConfirmation {
    CarrierConfirmation confirmation;
    std::uint32_t from_revision = 0;
};

// The member of a run's counts, and of its record "run:<run_id>" while it has
// not finished, that holds the confirmation.
inline constexpr const char* carrier_confirmed_member = "carrier_confirmed";
// The member of media_evidence.volume_json that holds a read of the device's
// own identity (device_identity_read_json).
inline constexpr const char* device_identity_member = "device_identity";

// The setting that caches the archive's confirmed device (a cache only: the
// truth is the history and the media evidence, which confirmed_device reads).
inline std::string device_identity_key(const Uuid& archive_uuid)
{
    return "device-identity:" + archive_uuid;
}

// "device_serial" or "operator", as the records hold it.
std::string_view confirmation_how_code(CarrierConfirmation::How how);

// {"how": ..., "device": {"model","serial","wwn"} (only where read),
// "enclosure_recorded": ..., "enclosure_now": ..., "from_revision": N}.
nlohmann::json carrier_confirmation_json(const CarrierConfirmation& confirmation, std::uint32_t from_revision);
// Nothing for a value that is no confirmation (another type, an unknown how,
// no revision); a device without a serial and a WWN is no read.
std::optional<RecordedConfirmation> carrier_confirmation_from_json(const nlohmann::json& json);
// The confirmation that `run` recorded in its counts, or nothing.
std::optional<RecordedConfirmation> carrier_confirmed(const VerifyRunRecord& run);
// The confirmation that a run's record ("run:<run_id>", parsed) carries.
std::optional<RecordedConfirmation> carrier_confirmed_in_record(const nlohmann::json& record);

// {"model","serial","wwn","how","read_at","device_ref"}, as the media
// evidence and the settings cache keep a read.
nlohmann::json device_identity_read_json(const DeviceIdentityRead& read);
std::optional<DeviceIdentityRead> device_identity_read_from_json(const nlohmann::json& json);
// The raw evidence of `volume` (volume_to_json, R-96) with the read of its
// device's own identity as the member device_identity_member.
std::string volume_json_with_device_read(const VolumeInfo& volume, const DeviceIdentityRead& read);
// The read that evidence's volume_json holds, or nothing.
std::optional<DeviceIdentityRead> device_read_of_evidence(const EvidenceRecord& evidence);

// Whether two reads name the same device: the same serial (as identity
// matching compares serials, a byte-pair-swapped one included), or the same
// WWN.
bool same_device_identity(const DeviceIdentity& left, const DeviceIdentity& right);

// The serial by which a carrier's enclosure is known: its one device's serial,
// or its WWN where it reports none; empty for a volume without one device.
std::string enclosure_serial(const VolumeInfo& volume);

// Whether a MIGRATED match of `now` with the carrier `recorded` is a changed
// enclosure: the same filesystem (MIGRATED says so) on one device on each
// side, both behind a USB bridge (bus usb), or the device seen now behind an
// enclosure that hides its identity (PhysicalDeviceInfo::
// enclosure_hides_identity). Any other MIGRATED (two internal devices, a
// device taken out of its enclosure, a spanned volume) is no enclosure's, and
// only a relocation accepts it.
bool enclosure_changed(const VolumeInfo& recorded, const VolumeInfo& now);

// The device confirmed as the archive's own, behind whatever enclosure: from
// the history, the device of the latest run whose confirmation recorded the
// new carrier (it passed and recorded revision from_revision + 1) with a read
// of the device; from the media evidence, the read recorded with a
// registration of the archive (its evidence with no run, noted "registered");
// the later of them, in the order the media evidence was recorded (each
// recorded run has its evidence, written with it). A relocation recorded after
// it ends it: the archive is on another carrier since, whose device nobody
// confirmed. `runs` and `evidence` of one archive (ArchiveStore::verify_runs,
// ArchiveStore::archive_evidence).
struct ConfirmedDevice {
    DeviceIdentity device;
    Timestamp confirmed_at;  // when: the run's end, or the registration's evidence
    std::string run_id;      // the run whose confirmation recorded it; empty: read at a registration
};
std::optional<ConfirmedDevice> confirmed_device(
    std::span<const VerifyRunRecord> runs, std::span<const EvidenceRecord> evidence);

// {"model","serial","wwn","confirmed_at","run_id"}: the value of the setting
// device_identity_key, which registration and a run that recorded the new
// carrier write; nothing for a value that names no device.
nlohmann::json confirmed_device_json(const ConfirmedDevice& device);
std::optional<ConfirmedDevice> confirmed_device_from_json(const nlohmann::json& json);

// confirmed_device of the archive, from its runs and its media evidence in
// `store`.
std::expected<std::optional<ConfirmedDevice>, IoError> read_confirmed_device(
    ArchiveStore& store, const Uuid& archive_uuid);

} // namespace va
