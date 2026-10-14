#include "core/identity/matcher.h"

#include "common/unicode.h"

#include <algorithm>
#include <optional>

namespace va {
namespace {

enum class Comparison { Same, Different, Unavailable };

// §B1.3: compare 64-bit (or full) identifiers when both sides have them,
// otherwise the 32-bit short forms.
Comparison compare_filesystem(const VolumeInfo& seen, const VolumeInfo& recorded)
{
    if (!seen.filesystem_uuid.empty() && !recorded.filesystem_uuid.empty()) {
        return seen.filesystem_uuid == recorded.filesystem_uuid ? Comparison::Same
                                                                : Comparison::Different;
    }
    if (!seen.filesystem_uuid_short.empty() && !recorded.filesystem_uuid_short.empty()) {
        return seen.filesystem_uuid_short == recorded.filesystem_uuid_short
                   ? Comparison::Same
                   : Comparison::Different;
    }
    return Comparison::Unavailable;
}

bool has_hardware_id(const VolumeInfo& volume)
{
    return std::any_of(volume.devices.begin(), volume.devices.end(), [](const auto& device) {
        return !device.serial.empty() || !device.wwn.empty();
    });
}

std::string normalise_serial(std::string_view serial)
{
    std::string result;
    for (const char character : serial) {
        if (character == ' ' || character == '_' || character == '\t') {
            continue;
        }
        result += (character >= 'a' && character <= 'z')
                      ? static_cast<char>(character - 'a' + 'A')
                      : character;
    }
    return result;
}

bool same_device(const PhysicalDeviceInfo& seen, const PhysicalDeviceInfo& recorded)
{
    if (!seen.serial.empty() && serials_equal(seen.serial, recorded.serial)) {
        return true;
    }
    return !seen.wwn.empty() && normalise_serial(seen.wwn) == normalise_serial(recorded.wwn);
}

// Either a serial or a WWN match counts as a device match (§B10).
std::optional<std::pair<const PhysicalDeviceInfo*, const PhysicalDeviceInfo*>> matching_devices(
    const VolumeInfo& seen, const VolumeInfo& recorded)
{
    for (const auto& seen_device : seen.devices) {
        for (const auto& recorded_device : recorded.devices) {
            if (same_device(seen_device, recorded_device)) {
                return std::pair{&seen_device, &recorded_device};
            }
        }
    }
    return std::nullopt;
}

Comparison compare_devices(const VolumeInfo& seen, const VolumeInfo& recorded)
{
    if (!has_hardware_id(seen) || !has_hardware_id(recorded)) {
        return Comparison::Unavailable;
    }
    return matching_devices(seen, recorded) ? Comparison::Same : Comparison::Different;
}

bool differs_by_more_than_one_percent(std::uint64_t left, std::uint64_t right)
{
    if (left == 0 || right == 0) {
        return false;
    }
    const auto larger = std::max(left, right);
    const auto difference = larger - std::min(left, right);
    return difference * 100 > larger;
}

std::string normalise_model(std::string_view model)
{
    // udev writes "WDC_WD80EFAX"; Windows writes "WDC WD80EFAX".
    std::string result;
    for (const char character : model) {
        const char mapped = character == '_' ? ' ' : character;
        if (mapped == ' ' && (result.empty() || result.back() == ' ')) {
            continue;
        }
        result += (mapped >= 'a' && mapped <= 'z') ? static_cast<char>(mapped - 'a' + 'A') : mapped;
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

// Row of the §B10 table for one candidate, before tie-breaking between candidates.
MatchResult classify(const VolumeInfo& seen, const VolumeInfo& recorded)
{
    const auto filesystem = compare_filesystem(seen, recorded);
    const auto devices = compare_devices(seen, recorded);
    if (filesystem == Comparison::Same) {
        if (devices == Comparison::Same) {
            return MatchResult::Match;
        }
        return devices == Comparison::Unavailable ? MatchResult::MatchWeak : MatchResult::Migrated;
    }
    // Decision 0018: the filesystem id cannot be read on one side (e.g. Linux
    // without the udev database) but the device matches. The volume's size is
    // then the only evidence about the filesystem: the same size is the same
    // volume, recognised weakly; another size is another filesystem on that
    // device, such as a second partition.
    if (filesystem == Comparison::Unavailable && devices == Comparison::Same) {
        return differs_by_more_than_one_percent(seen.capacity_bytes, recorded.capacity_bytes)
                   ? MatchResult::Mismatch
                   : MatchResult::MatchWeak;
    }
    return devices == Comparison::Same ? MatchResult::Mismatch : MatchResult::Unknown;
}

// R-95: secondary evidence adds warning details; a device whose capacity changed
// by more than 1 % behind a matching serial means the device was replaced.
void add_secondary_details(const VolumeInfo& seen, const VolumeInfo& recorded,
    MatchOutcome& outcome)
{
    if (compare_filesystem(seen, recorded) == Comparison::Unavailable) {
        outcome.details.push_back("filesystem-id-unavailable"); // decision 0018
    }
    if (seen.volume_label != recorded.volume_label) {
        outcome.details.push_back("volume-label-differs");
    }
    if (differs_by_more_than_one_percent(seen.capacity_bytes, recorded.capacity_bytes)) {
        outcome.details.push_back("capacity-differs");
    }
    if (seen.filesystem_type != recorded.filesystem_type) {
        outcome.details.push_back("filesystem-type-differs");
    }
    if (!seen.devices.empty() && !recorded.devices.empty() &&
        normalise_model(seen.devices.front().model) !=
            normalise_model(recorded.devices.front().model)) {
        outcome.details.push_back("model-differs");
    }
    if (const auto pair = matching_devices(seen, recorded);
        pair && differs_by_more_than_one_percent(pair->first->capacity_bytes,
                    pair->second->capacity_bytes)) {
        outcome.details.push_back("device-replaced");
        outcome.result = MatchResult::Mismatch;
    }
}

bool is_positive(MatchResult result)
{
    return result == MatchResult::Match || result == MatchResult::MatchWeak ||
           result == MatchResult::Migrated;
}

} // namespace

std::string_view to_string(MatchResult result)
{
    switch (result) {
    case MatchResult::Match:
        return "MATCH";
    case MatchResult::MatchWeak:
        return "MATCH_WEAK";
    case MatchResult::Migrated:
        return "MIGRATED";
    case MatchResult::Mismatch:
        return "MISMATCH";
    case MatchResult::Unknown:
        return "UNKNOWN";
    case MatchResult::Ambiguous:
        return "AMBIGUOUS";
    case MatchResult::PreviousCarrier:
        return "PREVIOUS_CARRIER";
    }
    return "UNKNOWN";
}

bool serials_equal(std::string_view left, std::string_view right)
{
    const auto a = normalise_serial(left);
    const auto b = normalise_serial(right);
    if (a.empty() || b.empty() || a.size() != b.size()) {
        return false;
    }
    if (a == b) {
        return true;
    }
    if (a.size() % 2 != 0) {
        return false;
    }
    for (std::size_t index = 0; index < a.size(); index += 2) {
        if (a[index] != b[index + 1] || a[index + 1] != b[index]) {
            return false;
        }
    }
    return true;
}

MatchOutcome match_identity(
    const MediaEvidence& evidence,
    std::string_view root_subpath,
    std::span<const ArchiveCandidate> candidates)
{
    const auto wanted_root = unicode_fold(root_subpath);
    std::vector<const ArchiveCandidate*> same_root;
    for (const auto& candidate : candidates) {
        if (unicode_fold(candidate.root_subpath) == wanted_root) {
            same_root.push_back(&candidate);
        }
    }

    MatchOutcome outcome;
    std::vector<std::pair<const ArchiveCandidate*, MatchResult>> positive;
    const ArchiveCandidate* mismatched = nullptr;
    for (const auto* candidate : same_root) {
        const auto result = classify(evidence.volume, candidate->identity);
        if (is_positive(result)) {
            positive.emplace_back(candidate, result);
        } else if (result == MatchResult::Mismatch && !mismatched) {
            mismatched = candidate;
        }
    }

    const ArchiveCandidate* selected = nullptr;
    if (positive.size() > 1) {
        outcome.result = MatchResult::Ambiguous;
        for (const auto& [candidate, result] : positive) {
            outcome.archives.push_back(candidate->archive_uuid);
        }
        return outcome;
    }
    if (positive.size() == 1) {
        selected = positive.front().first;
        outcome.result = positive.front().second;
    } else if (mismatched) {
        selected = mismatched;
        outcome.result = MatchResult::Mismatch;
    } else {
        // An earlier carrier is recognised at the root it had then.
        for (const auto& candidate_value : candidates) {
            const auto* candidate = &candidate_value;
            for (const auto& previous : candidate->previous_carriers) {
                if (unicode_fold(previous.root_subpath) != wanted_root) {
                    continue;
                }
                const auto result = classify(evidence.volume, previous.identity);
                if (result == MatchResult::Match || result == MatchResult::MatchWeak) {
                    selected = candidate;
                    outcome.result = MatchResult::PreviousCarrier;
                    break;
                }
            }
            if (selected) {
                break;
            }
        }
    }

    if (selected) {
        outcome.archives.push_back(selected->archive_uuid);
        if (outcome.result != MatchResult::PreviousCarrier) {
            add_secondary_details(evidence.volume, selected->identity, outcome);
        }
        if (selected->retired) {
            outcome.details.push_back("retired");
        }
    }

    // Last row of §B10: an identity file that names a different archive than the
    // evidence selected was copied from other media.
    if (evidence.claimed_archive_uuid) {
        const auto& claimed = *evidence.claimed_archive_uuid;
        if (selected && claimed != selected->archive_uuid) {
            outcome.result = MatchResult::Mismatch;
            outcome.details.push_back("identity-file-copied");
        } else if (!selected) {
            const auto known = std::find_if(candidates.begin(), candidates.end(),
                [&](const ArchiveCandidate& candidate) { return candidate.archive_uuid == claimed; });
            if (known != candidates.end()) {
                outcome.result = MatchResult::Mismatch;
                outcome.archives.push_back(claimed);
                outcome.details.push_back("identity-file-copied");
            }
        }
    }
    return outcome;
}

} // namespace va
