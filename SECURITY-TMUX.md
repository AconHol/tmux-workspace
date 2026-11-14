# Reporting security issues

Do not post passwords, private SSH keys, signing keys, tokens, unredacted SSH configuration or crash dumps in public issues.

For a suspected vulnerability in Tmux Workspace, report it privately using this repository's GitHub security advisory page when available. For upstream Windows Terminal issues, follow Microsoft's upstream reporting policy instead.

The app uses Windows OpenSSH for authentication and host verification. It does not store passwords, copy private keys, automatically accept unknown hosts or run pasted connection strings through a local shell. Remote tmux operations are shell-quoted and require normal SSH authorization.

Published MSIX files may use a self-signed developer certificate. Verify its identity and trust decision on your device; the portable archive does not require certificate installation. No private certificate material is included.
