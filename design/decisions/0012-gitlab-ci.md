---
status: accepted
date: 2026-10-08
amends: HANDOFF R-120
---

# CI runs on GitLab

The repository is hosted on GitLab (`gitlab.com:jjasoncool/veriarchive`), while R-120 named GitHub Actions (`windows-latest` with `msys2/setup-msys2`, `ubuntu-24.04`, a Fedora container). The same three environments run as GitLab CI jobs in `.gitlab-ci.yml`: Windows on GitLab-hosted Windows runners with MSYS2 UCRT64 installed by Chocolatey, Ubuntu 24.04 and Fedora as container images. The checks are unchanged: configure, build, unit and integration tests, the R-122 forbidden-construct grep, and on Windows the R-121 `ntldd` check.

## Consequences

- A milestone counts as done only after this pipeline passes on all three jobs, which requires pushing to GitLab.
- The Linux loop-device / `dm-flakey` test of R-120 needs a privileged runner and is not part of the default pipeline.
