# bare-windows

> **Status:** not implemented yet. The stack below is a proposal awaiting sign-off; see the pull request for the reasoning and the evidence behind it.

An example Windows app that embeds the [Bare](https://github.com/holepunchto/bare) runtime (via [bare-kit](https://github.com/holepunchto/bare-kit)) in a native host. It runs a shared on/off switch: launch two copies and flipping the switch in one flips it in the other - peer-to-peer over a distributed hash table, with no server. It is the Windows counterpart of [bare-macos](https://github.com/holepunchto/bare-macos) and [bare-linux](https://github.com/holepunchto/bare-linux).

The peer-to-peer half is JavaScript - a Hyperswarm node plus the switch state - shared as the [bare-switch-core](https://github.com/holepunchto/bare-switch-core) package and run on a Bare _worklet_ (a background JS thread the host starts). The native half is C: it boots the worklet and exchanges messages with it over bare-kit's IPC channel, over the same typed RPC stack bare-linux uses.

The host is plain Win32 with Common Controls v6, built with CMake via `bare-make`. There is no WinUI 3 or Windows App SDK dependency, so it builds and runs on a stock Windows install with nothing but a toolchain and Node - no runtime for users to install first.

## License

Apache-2.0
