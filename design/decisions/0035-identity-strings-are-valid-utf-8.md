---
status: accepted
date: 2026-10-27
amends: HANDOFF §B1.2 (the strings of `VolumeInfo` and `PhysicalDeviceInfo` are valid UTF-8), §B9 (`IMediaEnumerator` gives them so)
---

# Identity strings from the platform are made valid UTF-8

The platform describes a volume and its devices with strings as the hardware or the filesystem holds them. On Windows the storage descriptor's model and serial are the bytes a device or its USB bridge reports: usually ASCII, but not always text at all. On Linux, udev escapes the bytes of a label or a model that it cannot show as text (`ID_FS_LABEL_ENC`, `ID_MODEL_ENC`: `\xHH`), and VeriArchive turns them back into those bytes: the label of a FAT volume written by an older Windows in its code page (Big5 in Taiwan) is no UTF-8. sysfs shows a device's model as the device reports it. These strings go into the manifest header (`identity`, §B4.3), one canonical UTF-8 line (§B4.1, R-64), and into the media evidence as JSON (R-96). A header holding such bytes cannot be read back by the JSON parser, so its manifest could never be used, and the media evidence cannot be written at all, as the JSON library refuses to write bytes that are no UTF-8: a registration of such a volume read every file and then failed at its record, on every resume again, and a verification failed at its record the same way.

## Decision

1. Every identity string of a volume and its devices that the platform gives (`IMediaEnumerator::volumes` and `volume_for_path`, on Windows and on Linux) is valid UTF-8: the filesystem type and id (full and short form), the volume label and GUID, the container, and of each device its reference, model, serial, WWN and bus; the mount's `driver` as well (decision 0006), which is no identity but is shown in the report of a verification. Each byte that belongs to no valid UTF-8 sequence becomes U+FFFD (the replacement character, shown as �), as in a file name that is no valid Unicode (R-41's `p`); valid text, Chinese included, stays as it is.
2. Both platforms apply one shared function, `with_valid_identity_text` (`core/identity/identity_text.h`), to every volume they describe, before anything else sees it. The same bytes read on Windows and on Linux give the same text, so matching (§B10) compares like with like.
3. The original bytes are not kept. Identity strings are evidence for recognising a volume and its devices, compared as text: the replaced form recognises them as well as the bytes did, and the secondary strings (label, model) only add a warning detail where they differ (R-95).

## Why

Only valid UTF-8 can be written into a manifest that must be read back (R-62) and into the JSON of the media evidence. Refusing such a volume would leave its archive unprotected for a byte in a label or a model. Keeping the raw bytes beside the text, as R-41 does for names, whose bytes are a file's identity, would add header keys (a format change, §B4.3) for strings that only help to recognise a volume and its devices. The replacement is made where the strings enter VeriArchive, in the platform layer, so that core, the repository and both user interfaces never meet bytes that are no text.

Rejected alternatives:

- Escaping the bytes (`\xHH`, as udev does): text shown to the operator would carry escapes, and both platforms would have to escape alike to keep one volume one identity.
- Making the manifest writer and the evidence JSON tolerate such bytes instead: the header must stay one UTF-8 file (§B4.1), and every reader of the identity (matching, `archive show`, the GUI) would still meet bytes that are no text.

## Consequences

- A volume is registered and verified where its label, or a model or serial of its devices, holds bytes that are no text; its header shows U+FFFD in their place, the same on both systems.
- No manifest needs converting: a registration of such a volume never reached its record. A header holding only valid UTF-8 is unchanged byte for byte, and the golden tests are unchanged.
- Tests: the shared function with sample strings (a Big5 label, a model with a stray byte, a serial cut in the middle of a character): each such byte U+FFFD, valid text unchanged, and a header and the media evidence written with them read back; and every string the function makes valid, of the volume and of each of two devices, each given an invalid byte of its own. That each platform applies it is reviewed, not tested: the bytes come from devices that no test machine has.
