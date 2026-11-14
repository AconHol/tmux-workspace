# Contributing to Tmux Workspace

Open an issue with the Windows version, app version, connection method (SSH config, key or agent), tmux version, and minimal reproduction steps. Do not attach passwords, private keys, SSH configuration containing private endpoints, or unreviewed crash dumps.

This is an independent Windows Terminal fork. Workspace implementation is mainly in `src/cascadia/TerminalApp/Tmux*.cpp`, `SshCommandParser.h`, `ContentManager.*` and `TerminalPage.*`.

Use the upstream coding conventions. Preserve terminal behavior and native WinUI accessibility. Avoid introducing local-shell entry points into the SSH workspace.

Build and run the checks described in [README.md](README.md). Changes to tabs, focus or window lifetime require running the actual workspace workflow against a disposable Linux tmux server; passing parser checks alone is insufficient. That workflow creates temporary sessions and must never send input into user-owned panes.

Preserve MIT notices and third-party notices. Do not commit generated runtime settings, credentials, private signing keys, or build outputs. Contributions are licensed under MIT.
