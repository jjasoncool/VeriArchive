#pragma once
// What retiring an archive means (decision 0003), worded in `text`'s
// language: where its latest manifest is lost (decision 0028, point 9), what
// retiring it declares, which `archive retire` says before --confirm and the
// window before its explicit confirmation (decision 0046).
#include "app/text.h"
#include "core/engine/retire_archive.h"

#include <string>

namespace va::app {

// For a retirement whose latest manifest is lost (Retirement::lost_manifest):
// "Lost manifest of L (U), revision N", then, indented, what is wrong with it,
// then what retiring the archive declares and what to do otherwise (put it
// back with `import`, or register the next revision over it); one line each,
// each ending with a newline. Empty for a retirement whose manifest is not
// lost.
std::string retirement_explanation(const Text& text, const Retirement& retirement);

} // namespace va::app
