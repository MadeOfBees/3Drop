# Contributing

3Drop is a one-person project, so replies and reviews come as time allows.

- **A bug:** use the [bug report form](https://github.com/MadeOfBees/3Drop/issues/new/choose). A security problem goes through [SECURITY.md](SECURITY.md) instead. Never paste your key, `.env` or `key.txt`, and never commit them.
- **A change:** open an issue first for anything beyond a small fix, so we agree it fits before you write it.
- **The code:** [src/README.md](src/README.md) maps the app's modules and the way a request goes through them.
- **Build, test and lint:** the docs' [Building](docs/building.md#building) and [Tests](docs/building.md#tests) sections say how. `tools/check.sh` is the gate CI runs, and a change must pass it.
- **On a console or not:** the gate runs on the host against a fake device. Say in your pull request whether you also ran the change on a 3DS.
- **License:** contributions are under the project's license, [GPL-3.0-or-later](LICENSE). The app's own C sources each open with an SPDX license line. The host tests and the scripts have none (a script's first comment block is its usage text, which `-h` prints), and LICENSE covers them all the same.
