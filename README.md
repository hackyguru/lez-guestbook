<div align="center">

<img src="guestbook-ui/icons/guestbook.png" alt="LEZ Guestbook logo" width="128" height="128">

# LEZ Guestbook

**A guestbook on the Logos Execution Zone that everyone shares.**

A community-built, **unofficial** [Logos Basecamp](https://logos.co) module.
Sign it with a name and a message; anyone with the module, on any machine, sees
the same book. It's one step up from
[LEZ Counter](https://github.com/hackyguru/lez-counter): variable-size records,
signed authors, one account per entry, and two people writing at the same
moment.

![Basecamp](https://img.shields.io/badge/Logos%20Basecamp-0.2.3-2e7d5b)
![Network](https://img.shields.io/badge/network-LEZ%20testnet%20v0.3-6f42c1)
![Platform](https://img.shields.io/badge/platform-macOS%20arm64%20%7C%20Linux%20x86__64-lightgrey)
![Status](https://img.shields.io/badge/status-experimental-orange)
![Unofficial](https://img.shields.io/badge/module-unofficial-red)

</div>

> [!WARNING]
> **Unofficial, experimental, testnet only.** This is not an official Logos
> module and is not affiliated with or endorsed by Logos or IFT. It talks to
> the public LEZ v0.3 testnet, whose LGO has no value and which can be reset at
> any time. Entries are public and permanent: there is no moderation and
> nothing can be deleted.

---

## Contents

- [Quick start](#quick-start)
- [Live deployment](#live-deployment)
- [How it works](#how-it-works)
- [Build from source](#build-from-source)
- [Verifying it independently](#verifying-it-independently)

## Quick start

1. Download `logos-guestbook_core-module-lib.lgx` and `logos-guestbook-module.lgx`
   from the [latest release](https://github.com/hackyguru/lez-guestbook/releases/latest).
   Each holds both macOS (Apple Silicon) and Linux (x86_64) builds.
2. In Basecamp, open **Modules → Install LGX Package** and install the core
   first, then the UI.
3. Open **guestbook** in the sidebar, write a name and a message, and sign.
   The module fetches the LGO it needs for fees from the testnet faucet by
   itself, so there's nothing to configure.

The packages contain the program address, so everyone who installs them reads
and signs the same book. Releases are built by
[`.github/workflows/release.yml`](.github/workflows/release.yml) whenever a
`v*` tag is pushed.

## Live deployment

| | |
|---|---|
| Program | `Edc6RDHB6bjCUfseKfff7jkhEwWqDHuBieWpE2YiyHq` |
| Header account (signature count) | `DkshwcR5xRf4bFvmugbBxt8P5rCzVvyUmJVHs9LPmEjp` |
| Image ID | `00e8aa77af3c139951898a26f5c836a1450445ca917c55b164fbef950543df84` |
| Deployed | 2026-10-04: 3 segments + header, ~6 min |

Verified with two separate wallets:
- The deployer signed (#1).
- A fresh wallet pointed at the address and signed (#2).
- Then **both signed at the same moment**. One got slot #3. The other's
  transaction was rejected by the program, and the module retried and
  landed at #4. Nothing was lost or overwritten.
- `./verify.sh` independently derives every entry address and decodes all
  four from the chain.

## How it works

**Storage: one account per entry.** An LEZ account holds at most 100 KiB, and
rewriting one big blob would make every post cost more than the last. So:

```
header   PDA(program, "guestbook/header")          u64 LE — number of entries
entry n  PDA(program, "guestbook/entry/" ‖ n_le)   Borsh Entry { author: [u8;32], name, text, time: u64 }
```

**Posting** sends the program three accounts: header, entry *n*, and the
author. The program then:

1. **Checks the header and entry are the right derived accounts.** It derives
   them itself, so nobody can scribble entries into arbitrary accounts.
2. **Requires the author's signature** and records the author's address in
   the entry. Names are free text, but the address beside each name can't be
   faked.
3. **Bumps the header only if it still says *n*, and writes the entry only if
   the slot is empty.** Two posts racing for slot *n* can't both win. The
   loser's transaction lands, pays its fee and changes nothing; the module
   sees its entry isn't there and retries at *n + 1*.

Names are capped at 40 bytes and messages at 500, and entries are immutable,
so the module caches them for good and only fetches what's new. The `time`
on an entry is the poster's clock, unverified (fine for a guestbook).

**There's no moderation.** Nothing can be deleted, by design. If that matters
for an event, a moderated version would add an owner key that can mark
entries hidden (the UI hides them; the chain still has them).

## Build from source

```
lez-guestbook/
├── guestbook-program/  the on-chain program (Rust → risc0 guest), build.sh, guestbook.bin
├── guestbook-core/     guestbook_core — universal C++ module, links the LEZ wallet FFI
│   └── tests/          harness: deploy / use / post / state against the testnet
├── guestbook-ui/       guestbook — QML (compose, signatures, details)
├── verify.sh           read the book straight from the chain with curl + python3
└── install.sh          drop both into a local Basecamp
```


```bash
cd guestbook-program && ./build.sh       # Docker running; → guestbook.bin
cd ../guestbook-core
GUESTBOOK_DATA_DIR=~/gb-deployer tests/run.sh deploy ../guestbook-program/guestbook.bin   # only after a reset
GUESTBOOK_DATA_DIR=~/gb-alice    tests/run.sh post "Alice" "hello"                      # try it from a CLI wallet
nix build 'path:.#lgx-portable' -o result-portable
cd ../guestbook-ui && nix build 'path:.#lgx-portable' -o result-portable --override-input guestbook_core path:../guestbook-core
cd .. && ./install.sh
```

The UI pins its core to this repo on GitHub; the `--override-input` above
builds it against your checkout instead.

## Verifying it independently

`./verify.sh` reads the book straight from the testnet sequencer: no Basecamp,
no module, no wallet. It derives every entry's account address itself and
decodes the newest ten, so if the module and the script agree, the entries
really are on the chain.

Testnet resets wipe deployed programs. Redeploy (above), then update
`kDefaultProgram` in `guestbook-core/src/guestbook_impl.cpp` or use the
module's Details view. The build and deploy gotchas are the same as for the
counter; see [LEZ Counter's README](https://github.com/hackyguru/lez-counter#things-that-bit-so-you-dont-have-to).
