# Building PLANK Linux Host

Build this component through the [parent PLANK repository](https://github.com/instinctual/plank).
It supplies the exact source pins, prepared dependencies, transport library,
Rocky Linux 9.7 container, test gates and explicit RPM manifest.

- [Build from source](https://github.com/instinctual/plank/blob/main/docs/development/build/from-source.md)
- [Release build runbook](https://github.com/instinctual/plank/blob/main/docs/development/build/release-build-runbook.md)
- [GitHub-hosted builders](https://github.com/instinctual/plank/blob/main/docs/development/build/github-builds.md)
- [Hardware acceptance](https://github.com/instinctual/plank/blob/main/docs/development/acceptance-criteria.md)

The supported Linux encoders are software x264 (`software-cuda`) and direct
NVENC (`nvenc-direct`). The Client bookmark chooses the exact profile; there is
no fallback to another codec or a generic encoder backend. The qualified Linux
product requires CUDA, including when x264 encodes the captured frames on CPU.

The inherited CPack, AppImage, Flatpak, Homebrew and generic distribution build
scripts are retired. Do not install broad capabilities or start the media
worker as root to bypass the parent package's supervisor/PAM/input separation.

The macOS Host is a separate native implementation under `apps/host/macos` in
the parent repository. The old macOS backend in this component is removed.
Other inherited platform source is not a qualified PLANK package.
