# Reporting security issues

Do not post passwords, private SSH keys, signing keys, tokens, unredacted SSH configuration or crash dumps in public issues.

For a suspected vulnerability in Tmux Workspace, report it privately using this repository's GitHub security advisory page when available. For upstream Windows Terminal issues, follow Microsoft's upstream reporting policy instead.

The app uses Windows OpenSSH for authentication and host verification. It does not store passwords, copy private keys, automatically accept unknown hosts or run pasted connection strings through a local shell. Remote tmux operations are shell-quoted and require normal SSH authorization.

Published MSIX files may use a self-signed developer certificate. Verify its identity and trust decision on your device; the portable archive does not require certificate installation. No private certificate material is included.

AI translation is opt-in per request and uses the endpoint/model configured by the user. Only the translation form's source text and target language are sent as translation content; terminal buffers are not collected. API secrets in tmux-ai.json are encrypted with Windows DPAPI for the current user. This is local credential protection, not isolation from other processes running as that same user. Runtime settings and encrypted secrets are excluded from releases. API requests require HTTPS except for loopback development services; automatic redirects are disabled. Model output containing terminal control bytes is rejected and translated newlines are joined, so insertion does not submit the shell input. The original terminal connection is rechecked after the request, and cancelled results are discarded.
