# Changelog

## [0.2.2](https://github.com/matjam/os9-enforcer/compare/v0.2.1...v0.2.2) (2026-11-16)


### Bug Fixes

* with virtual memory on, no pool growth while the File Manager is busy ([61cda2b](https://github.com/matjam/os9-enforcer/commit/61cda2bf1ab8a83a1669bcae3163aac2792423e0))

## [0.2.1](https://github.com/matjam/os9-enforcer/compare/v0.2.0...v0.2.1) (2026-11-16)


### Bug Fixes

* the page pool no longer grows without end under an application that reuses its heap ([b832e2a](https://github.com/matjam/os9-enforcer/commit/b832e2a5f087cae957a226ad8bec5a4cd513131e))

## [0.2.0](https://github.com/matjam/os9-enforcer/compare/v0.1.0...v0.2.0) (2026-11-15)


### Features

* About This Computer shows the memory an application has taken, not its 29 MB partition ([2253751](https://github.com/matjam/os9-enforcer/commit/2253751789abf90c911409183362065b9dfe1802))
* the version is in each program's Get Info and in the control panel's and installer's title bars ([2253751](https://github.com/matjam/os9-enforcer/commit/2253751789abf90c911409183362065b9dfe1802))


### Bug Fixes

* do not stop an application for a store into another's stack ([2253751](https://github.com/matjam/os9-enforcer/commit/2253751789abf90c911409183362065b9dfe1802))
* writing files from on-demand memory failed with virtual memory on, so an update could not be installed ([2253751](https://github.com/matjam/os9-enforcer/commit/2253751789abf90c911409183362065b9dfe1802))

## 0.1.0 (2026-11-15)


### Features

* releases with release-please, as a StuffIt archive in BinHex ([938e5da](https://github.com/matjam/os9-enforcer/commit/938e5da57e837688c86c95204ddb94257a8343f9))
