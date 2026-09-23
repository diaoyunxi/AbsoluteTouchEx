# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 2.x     | :white_check_mark: |
| < 2.0   | :x:                |

## Reporting a Vulnerability

If you discover a security vulnerability in AbsoluteTouchEx, please report it responsibly.

**DO NOT** open a public GitHub issue for security vulnerabilities.

Instead, please:
1. Email the maintainer directly (check git log for contact info)
2. Include detailed steps to reproduce
3. Allow reasonable time for a fix before public disclosure

## Security Considerations

AbsoluteTouchEx uses DLL injection to function. This is by design, but users should be aware:

- Only run `atloader.exe` from trusted sources
- Verify DLL checksums before use
- The injected DLL (`atdll.dll`) only processes touchpad input and does not access network or sensitive data
- Some anti-cheat systems may flag this behavior

## Scope

The following are considered in-scope security issues:
- DLL injection vulnerabilities
- Privilege escalation
- Input validation bypass
- Buffer overflows in HID parsing

Out of scope:
- Anti-cheat detection (this is expected behavior)
- Issues in third-party dependencies (Detours, nlohmann/json)
