---
status: accepted
date: 2026-11-02
amends: HANDOFF §B2 (R-44: the OS that registered the revision is the one that recorded it), decision 0008 (the registering OS is the OS that recorded the revision), decision 0017 (the words for `MediaIdentityMismatch` and for what identity matching noted), decision 0020 (the operating systems by which a link's absolute target is judged), decision 0028 (the confirmation of a registration over a lost manifest)
---

# What a revision records is not called registered

Decision 0041 found that Compare Archives called the time of a compared revision "registered", though a relocation (decision 0004) and a FULL verify that adds chunk hashes (decision 0033) record revisions too. Other texts said the same of what an archive's current revision records:

- A verification's words for `MediaIdentityMismatch`, "the media is not the one registered", and for what identity matching noted, "identity: the volume label differs from the one registered" (and so the capacity, the filesystem type and the device model), compare the media with the identity of the revision verified. That identity describes the carrier the revision was read from (HANDOFF §B4.3, decision 0004): a relocation records the new carrier's, and a verification adding chunk hashes the one it read.
- The confirmation of a registration over a lost manifest says "damage since revision N was registered would not be found" of the archive's current revision, whatever recorded it.
- The rules for a link's absolute target and for a name that cannot exist on the verifying OS (decision 0008, R-44) follow the OS that recorded the revision, its `created_by.os`, which the records and the user guides called the OS that registered it.
- The user guides said that the Archive List's Volume, Model and Capacity are those recorded at registration, though the list shows the identity that the archive's current revision records (decision 0028, point 5).

## Decision (operator, 2026-11-02)

1. These texts say "recorded" where they said "registered": "the media is not the one recorded" (媒體不是記錄中的那一個); "identity: the volume label differs from the one recorded" (媒體身分：磁碟區標籤與記錄中的不同), and so the capacity (容量), the filesystem type (檔案系統類型) and the device model (裝置型號); "damage since revision N was recorded would not be found" (自版次 N 記錄以來發生的損壞將不會被發現).
2. The records and the user guides name the OS of decision 0008's rules as the OS that recorded the revision (`created_by.os`): that of its registration, or of the relocation or the verification adding chunk hashes that recorded it. The rules do not change. A relocation records links as it read them (decision 0032, point 5), and a verification adding chunk hashes records the next revision only where every entry matched as recorded (decision 0033), so no entry it carries was read otherwise on another OS.
3. Texts about a registration itself keep "registered": what a registration read or could not read (an entry unreadable at registration, decision 0010, which a relocation and adding chunk hashes carry unchanged), an archive's registration (decision 0040), and the scope sentence of a comparison (decision 0041, point 3).
4. No change to the manifests, the database or the golden test; `--json` keeps the failure classes and the codes of what matching noted.

## Why

The operator chose one word for what a revision records, whatever recorded it: "registered" is true only of a revision that a registration recorded, and elsewhere it names a registration that never read what is compared.

## Consequences

- Decision 0008: the registering OS of its title is the OS that recorded the revision; its Names bullet and its amends line say so.
- Decision 0017: the words it quotes say so.
- Decision 0020: its table of statuses and its bullet about Windows and Linux say so.
- Decision 0028: its confirmation, the reasons for it and its interface terms say so.
- The user guides: the Archive List's Volume, Model and Capacity, the registration over a lost manifest, the `Unrepresentable` row of a verification's statuses and the `MetadataMismatch` row of a comparison's.
- Comments in the code and the tests that said the same say "recorded" too.
- Tests: the words of the four notes of identity matching in the engine, each from a sample identity that differs in it, and in Chinese; the words of `MediaIdentityMismatch` in English and in Chinese; the confirmation over a lost manifest in English and in Chinese, and end to end in English. A mutation check put "registered" back in each text in turn (the four notes in the engine and in Chinese, the media's words in English and in Chinese, the confirmation in English and in Chinese), and each failed the test meant for it.

### Interface terms (decision 0017)

| English | 繁體中文 |
|---|---|
| the media is not the one recorded | 媒體不是記錄中的那一個 |
| identity: the volume label differs from the one recorded | 媒體身分：磁碟區標籤與記錄中的不同 |
| identity: the capacity differs from the one recorded | 媒體身分：容量與記錄中的不同 |
| identity: the filesystem type differs from the one recorded | 媒體身分：檔案系統類型與記錄中的不同 |
| identity: the device model differs from the one recorded | 媒體身分：裝置型號與記錄中的不同 |
| damage since revision N was recorded would not be found | 自版次 N 記錄以來發生的損壞將不會被發現 |
