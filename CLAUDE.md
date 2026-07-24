# iPXE mDNS enhancement

**Living charter/lab notebook for branch `claude/mdns`.  Keep
the dated status section updated every session so any new session can
resume from this document alone.**

## Goal

Design and implement mDNS client and configuration for iPXE.

## Design notes

TBD

## Status

- **2026-07-24 (a)**: Initial commit with just this CLAUDE.md file

## Build & test

```sh
cd src
make -j$(nproc) bin-x86_64-efi/ipxe.efi
util/genfsimg -o ipxe.iso bin-x86_64-efi/ipxe.efi   # for BMC virtual media
```

## Workflow

- Branch: `claude/mdns` (based on `origin/master`; do not push
  elsewhere).  Keep commits small and buildable.
