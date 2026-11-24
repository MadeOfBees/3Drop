# Security policy

## Reporting a vulnerability

Report it privately, through GitHub's **Private vulnerability reporting**: open this repository's **Security** tab, choose **Report a vulnerability** ([the form](https://github.com/MadeOfBees/3Drop/security/advisories/new)) and fill it in. Please do not open a public issue or discussion for it. If the button is missing, open an issue that asks for a private contact, with no detail of the problem.

Say what you found, how to reproduce it and what it lets someone do, with your host and the release the tools and the console ran (`tools/device.sh --version`, and the release on 3Drop's top screen). Never put the pairing key in a report, in text or in a screenshot.

3Drop is a one-person project: reports are read as time allows, with no promised response time.

## Supported versions

Only the latest release gets fixes. A fix ships as the next release number, and the console and the tools are updated together.

## What is by design

The docs' [Safety](docs/safety.md#safety) section lists what the key holder can do and what an eavesdropper on your network sees. A report that only restates it is not a vulnerability.
