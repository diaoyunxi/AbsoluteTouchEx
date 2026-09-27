# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| Latest  | :white_check_mark: |

## Reporting a Vulnerability

### How to Report

1. **Do NOT open a public issue** for security vulnerabilities
2. Use GitHub's [private vulnerability reporting](https://github.com/diaoyunxi/) feature
3. Include detailed description and reproduction steps

### Response Timeline

- **48 hours**: Acknowledgment
- **7 days**: Assessment
- **30 days**: Fix for critical issues

### Scope

- Kernel driver vulnerabilities
- Privilege escalation vectors
- Memory safety issues (buffer overflows, use-after-free)
- DLL injection/hijacking risks

### Best Practices

- Only install signed drivers
- Verify DLL integrity before loading
- Run with minimum required privileges
