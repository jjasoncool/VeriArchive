#pragma once
// SMART in the words of the interfaces (decision 0052): the names of the ATA
// attributes and NVMe values, what a read says (its alerts and hints), why a
// read gave nothing, and how it was read. The repository keeps the pages and
// codes; these words are made from them each time.
#include "app/text.h"
#include "core/smart/smart_data.h"

#include <cstdint>
#include <string>
#include <optional>
#include <string_view>

namespace va::app {

// An ATA attribute's name: "Reallocated sectors" for 5; "Attribute 231"
// for one this does not name (its meaning is the maker's).
std::string attribute_name(const Text& text, std::uint8_t id);
// Its id as smartctl and CrystalDiskInfo show it: two uppercase hex digits.
std::string attribute_id(std::uint8_t id);

// One finding of a read: "Pending sectors increased (0 → 8)", "Reallocated
// sectors: 8 (as in the last read)", "UltraDMA CRC errors increased (3 → 12):
// usually a cable or an enclosure, not the device itself", "Pending sectors
// cleared (8 → 0)".
std::string finding_words(const Text& text, const SmartFinding& finding);

// Why a read gave no SMART (SmartRead::problem, with its detail): "The
// enclosure passes no ATA command on to the device." A read whose data does
// not decode: that its SMART data does not check.
std::string smart_problem_words(const Text& text, const SmartRead& read);

// How a read was read: "through its enclosure (SAT)", "directly (ATA)",
// "NVMe health log".
std::string smart_how_words(const Text& text, std::string_view how);

// What a device alert of SMART advises, after its lines: to copy the
// archives on the device to another device soon and verify the copies.
std::string smart_advice(const Text& text);

} // namespace va::app

namespace va::app {

// A SMART read as a run's result page lists it with the run's devices
// (decision 0052): "SMART OK · read 2026-11-10 14:03 (UTC+8), through its
// enclosure (SAT)"; "SMART alert: Pending sectors: 8; Reallocated sectors: 8 ·
// read ..."; or why it gave nothing.
std::string smart_read_words(const Text& text, const SmartRead& read);

// Its alert line, led by the alert mark, where it alerts: "⚠ SMART of
// SAMPLE MODEL (serial ...), read ...: Pending sectors: 8"; nothing otherwise.
std::optional<std::string> smart_read_alert_line(const Text& text, const SmartRead& read);

} // namespace va::app
