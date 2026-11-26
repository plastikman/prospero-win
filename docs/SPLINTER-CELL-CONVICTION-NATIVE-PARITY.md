# Splinter Cell: Conviction native Wine parity

This note maps the `dinput8.dll` workarounds attached to
[orbis-win issue #8](https://github.com/plastikman/orbis-win/issues/8) to the
Wine source series. The aim is to let the game run without a per-game proxy
DLL. The changes are on `codex/issue8-native-fixes` in the Prosperowin source
repository.

## Coverage

| Proxy workaround | Native source path | State |
|---|---|---|
| `GetAdaptersInfo` returns an error and Conviction walks an invalid result | `1009-iphlpapi-ps4-netctl-get-adapters-info.patch` returns the configured NetCtl Ethernet adapter when NSI is absent | Implemented; expects the launcher to pass `WINE_PS4_NETCFG` |
| Level-load job deadlock; lower the CPU tier only during loading | `1012-ntdll-honor-wine-cpu-topology-processor-count.patch` honors `WINE_CPU_TOPOLOGY` | Partial: a profile can lower the reported CPU count for the process, but this does not restore the full count after loading as the proxy does |
| No drive strings, so the game cannot find sound packages | `1010-kernelbase-find-roots-when-no-drives-are-published.patch` probes drive roots when `\\DosDevices\\` has no published letters | Implemented |
| DirectInput cannot enumerate the XInput-only controller | `1013-winebus-expose-title-controller-as-hid-gamepad.patch` exposes the title's controller sink as a HID gamepad | Source implemented; PS4 packaging now builds the `winebus.sys` driver and its `winebus.prx` Unix side. Console enumeration and play still need validation |
| MMDevices endpoint names gain nested wrappers on every start | `1011-mmdevapi-keep-persisted-audio-names.patch` leaves persisted properties alone while loading absent endpoints | Implemented |
| Six-channel WASAPI audio must be downmixed to stereo | Existing `src/pw_audio_mix.c` converts multichannel streams into the stereo console mix; `tests/test_pw_audio_mix.c` contains a 5.1 case | Existing native path; no additional proxy needed based on source inspection |

## Remaining validation

The loading-only CPU-tier behavior is the remaining gap. `WINE_CPU_TOPOLOGY=1`
is available as a per-title experiment, but it is static and can reduce worker
parallelism during gameplay. A console run must confirm whether Conviction
loads and remains playable with that setting before claiming the proxy is
unnecessary. Do not add the proxy's executable-address patch to Wine globally:
it only recognizes two executable hashes and mutates game code at runtime.

For the next console run, remove the `dinput8` override and the copied proxy,
then enable `WINE_CPU_TOPOLOGY=1` in the title profile. Confirm the log shows
the NetCtl adapter fallback, that the game sees the C: drive, and that the pad
appears through DirectInput. Check both loading progress and in-game controller
and audio behavior. This is an A/B validation of the native path, not evidence
that the dynamic CPU-tier workaround has been reproduced.

## Build verification

The five new source patches apply in order to Wine
`490f6d5dcbb2a5047345b8af88d114bbcaad69a8`. The touched `iphlpapi`,
`kernelbase`, and `mmdevapi` files compile for both 32-bit and 64-bit PE
targets; the changed `ntdll` source and Winebus source compile in the host
build configuration.

The duplicate `0904-win32u-mark-extended-keys-for-directinput.patch` has been
removed from the series: `0902-win32u-ps5-send-extended-keys-as-extended.patch`
already adds the same `is_extended_key()` helper and flag assignment. Stale
diagnostic patches 0975, 0992, 0995 and 0998 were rebased onto this Wine pin;
orphan patches 0993 and 0997 were removed because their target diagnostics are
absent from the pinned source. The ordered patch check now reaches 0999, which
still does not apply to this pin. A complete build and package validation
remain blocked until that archive is reviewed and rebased or removed.

`WINE_CPU_TOPOLOGY=1` is a static fallback, not a reproduction of the proxy's
loading-only tier switch. A fully equivalent no-proxy claim still requires a
PS4 run without the copied proxy, confirmation that the native Winebus PRX is
loaded, and a level-load/gameplay check. The original proxy's six-channel
downmix is covered by the existing PS4 audio mixer, which folds supported
multichannel layouts into the stereo console sink.
