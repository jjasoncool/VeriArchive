#pragma once
// The reads of a device's own identity behind its enclosure that the window
// keeps (decision 0047): each read the device identity reader made on the
// operator's request (the enclosure question of a FULL verify, Devices,
// Register Archive), by the device's system name, with the device as the
// system described it then and the archives whose confirmed device it is.
// Devices shows a read kept, and Register Archive records it without asking
// Windows for administrator permission once more, only while the system
// describes the device at that name exactly as it did then: another device
// given the same name, or the device taken out of its enclosure, is read
// again. That cannot tell another device of the same model put into the same
// enclosure or dock (all the system describes is the enclosure's), so the
// window drops every kept read once it has not watched the devices for a
// while (devices_poll.cpp), and the enclosure question of a FULL verify never
// takes one: it reads the device anew (enclosure_flow.h). And what the
// window says where a read gave nothing, in each place that asks for one.
//
// Plain C++ without GTK, so the unit tests can check it.
#include "app/text.h"
#include "common/error.h"
#include "common/types.h"
#include "core/interfaces.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace va::gui {

struct KeptDeviceRead {
    DeviceIdentityRead read;
    PhysicalDeviceInfo seen;         // the device as the system described it when it was read
    std::vector<Uuid> confirmed_of;  // the archives whose confirmed device it is: as read then (confirmed_device),
                                     // and those that recorded it since (note_confirmed)
};
using KeptDeviceReads = std::map<std::string, KeptDeviceRead>;  // by the device's system name (device_ref)

// Whether `now` is still the device described as `then`: the same system name
// (never an empty one), model, serial and WWN as the enclosure reports them,
// bus and capacity.
bool still_the_device(const PhysicalDeviceInfo& then, const PhysicalDeviceInfo& now);

// The kept read of `device`, while it is still the device read; nothing
// otherwise.
const KeptDeviceRead* kept_read_of(const KeptDeviceReads& reads, const PhysicalDeviceInfo& device);

// After a read of the attached devices: drops each kept read whose device
// is no longer among `attached` as it was described.
void keep_reads_of(KeptDeviceReads& reads, const std::vector<PhysicalDeviceInfo>& attached);

// `device` is the confirmed device of `archive` from now on: a verification
// that went on with a confirmation naming it passed and recorded the new
// carrier, or a registration recorded it. Each kept read of it says so.
void note_confirmed(KeptDeviceReads& reads, const DeviceIdentity& device, const Uuid& archive);

// Whether a read of `device`'s own serial is offered: its enclosure hides it
// (PhysicalDeviceInfo::enclosure_hides_identity), the system names the device
// (the reader is given that name), and this computer has a device identity
// reader (IDeviceIdentityHelper::available, never on Linux).
bool offers_device_read(const PhysicalDeviceInfo& device, bool reader);

// Why a read gave nothing (device_identity_failure), with the error's own
// message where the device could not be read. Nothing for a read cancelled:
// the operator closed what asked for it, and nothing is said.
struct ReadProblem {
    DeviceIdentityFailure failure = DeviceIdentityFailure::Unreadable;
    std::string cause;  // the error's message, an English sentence
    bool operator==(const ReadProblem&) const = default;
};
std::optional<ReadProblem> read_problem(const IoError& error);

// What the window says of it: in the enclosure question of a FULL verify,
// which stays open (and offers the operator confirmation then); on the
// Confirm page of Register Archive, which then registers without the
// serial; in a toast of Devices. Never the advice to attach the device
// directly: taken out of its enclosure, only a relocation accepts it.
enum class ReadShownIn { Question, Registration, Devices };
std::string read_problem_words(const app::Text& text, const ReadProblem& problem, ReadShownIn where);

// The serial a read names its device by: its serial, or its WWN where it
// reads none.
std::string read_serial(const DeviceIdentity& device);

} // namespace va::gui
