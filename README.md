# bare-windows

An example Windows app that embeds the [Bare](https://github.com/holepunchto/bare) runtime (via [bare-kit](https://github.com/holepunchto/bare-kit)) in a native C host. It runs a shared on/off switch: launch two copies and flipping the switch in one flips it in the other - peer-to-peer over a distributed hash table, with no server. It is the Windows counterpart of [bare-linux](https://github.com/holepunchto/bare-linux) and [bare-macos](https://github.com/holepunchto/bare-macos).

The peer-to-peer half is JavaScript - a Hyperswarm node plus the switch state - shared as the [bare-switch-core](https://github.com/holepunchto/bare-switch-core) package and run on a Bare _worklet_ (a background JS thread the host starts). The native half is C: it boots the worklet and exchanges messages with it over bare-kit's IPC channel, using the same typed RPC stack as bare-linux.

The host is plain Win32 with Common Controls v6, built with CMake via `bare-make`. There is no WinUI 3 or Windows App SDK dependency, so it builds and runs on a stock Windows install with nothing but a toolchain and Node - and there is no runtime for users to install first.

## Building and running

### Prerequisites

- A Windows machine on x64 or arm64 - the prebuilt runtime is published for both.
- Node 22+.
- [LLVM](https://releases.llvm.org) - `bare-make` builds with `clang-cl` and `llvm-lib`, and looks for them in `C:\Program Files\LLVM\bin` and on `PATH`. Visual Studio's C++ workload is not a substitute; CI pins LLVM 20.1.8. `bare-make` brings its own CMake and Ninja, and downloads the prebuilt runtime itself, so you do not install those.

Every build step takes its target from Node's own arch, so on an arm64 machine install the arm64 build of Node. The usual Node for Windows on ARM is x64 under emulation, and the whole build - runtime, addons, host - then quietly comes out x64.

### Build

From the repo root:

```sh
npm install
npx bare-make generate      # fetch the runtime, link addons, pack the worklet
npx bare-make build         # build the C host
.\build\app\bare_windows.exe
```

The first `generate` downloads the prebuilt runtime and caches it under `build/`, so later runs are fast. It is a large download - the release zip carries every platform - and it only happens once per build directory.

A window opens with the shared switch, the number of connected peers, and this instance's public key and topic. Launch a second copy - run `.\build\app\bare_windows.exe` again, or start it on another machine on the same network. Once the two find each other on the DHT - usually within a minute - the peer count shows 1, and flipping the switch in one window flips it in the other. There is no server in between.

The switch is deliberately naive - last-writer-wins with no conflict resolution. Flip both windows at nearly the same moment, or launch a fresh peer whose default state clobbers yours, and the two can end up disagreeing with no way to say whose state is right. That divergence is the point, not a bug: convergent multi-writer state is a different building block, [Autobase](https://github.com/holepunchto/autobase), which linearizes each peer's log into one deterministic view. This example keeps the switch naive to keep the focus on embedding Bare and talking to it over a typed protocol; [bare-macos](https://github.com/holepunchto/bare-macos) walks through the divergence in full.

CI builds the app on x64 and arm64; it does not launch it, since a GUI needs a desktop session.

### Example: a Windows 11 VM on an Apple Silicon Mac

The setup that worked for us - a Windows 11 arm64 guest under VMware Fusion, which is free for personal use:

1. Install VMware Fusion and create a Windows 11 arm64 VM.
2. In the guest, install Git, LLVM, and the arm64 build of Node 22+ from [nodejs.org](https://nodejs.org) - the download page offers an x64 installer by default, which is not what you want here.
3. Enable OpenSSH Server in the guest (Settings, Optional features) if you want to drive the build from the Mac. The window itself needs the guest's desktop, so watch the app there rather than over SSH.

Clone the repo inside the guest and run the [Build](#build) steps there. Do not share a `node_modules` from the Mac: it holds platform-specific native binaries, so `npm install` has to run in the guest.

## How it works

The build is driven entirely by CMake (via `bare-make`): it fetches the prebuilt runtime, links the worklet's native addons, and packs the bundle - all into `build/app`. The addons (`sodium-native`, `udx-native`, ...) are not linked into the host; the Bare runtime loads them at runtime. Windows has no rpath, so `bare-kit.dll` and the addons sit next to the exe, which is the first place the loader looks. bare-linux solves the same problem with a `$ORIGIN/lib` rpath.

One Windows-specific wrinkle is worth knowing about, because nothing else in the family needs it. A prebuilt addon does not link against the runtime directly: it delay-loads `bare.exe` or `bare.dll` and resolves those imports against whichever module in the process exports `bare_module_find`. In `bare.exe` that is the executable itself. Here the runtime lives in `bare-kit.dll`, so the host executable re-exports bare-kit's symbols as forwarders - `bare-kit.def`, which bare-kit ships alongside the DLL, is passed straight to the linker as `/DEF:`. Without it the addons fail to load, and the app would come up, print its identity, and then sit silent forever.

## License

Apache-2.0
