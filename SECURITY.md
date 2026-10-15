# Security policy

Wabcut is an alpha built by one developer, on Windows only. Security reports are welcome and taken seriously, but
please keep the size of the project in mind when you read the response times below.

## Supported versions

Only the latest release gets security fixes. Older versions are not patched; update to the latest release instead.

| Version | Supported |
| --- | --- |
| 0.1.x (latest) | Yes |
| Anything older | No |

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Report it privately through GitHub: open the [Security tab](../../security) of this repository and choose
**Report a vulnerability**. Include:

- the Wabcut version (shown in the title bar) and your Windows version;
- what an attacker can do, and what they need first (a crafted project or media file, a malicious extension, local
  access, a hostile AI provider response, ...);
- steps or a file that reproduces it, if you have one.

What to expect:

- an acknowledgement within **7 days**;
- an assessment, and a plan or a reason to decline, within **30 days**;
- a fix in the next release for confirmed issues, credited to you in the release notes unless you prefer otherwise.

There is no bug bounty.

## Scope

In scope: the Wabcut app and `wabcut-cli` from this repository, including

- opening untrusted files: projects (`.wabcut`), interchange files (FCPXML, Premiere XML, EDL, OTIO), keymaps, LUTs,
  caption files (SRT, VTT), Lottie animations and media;
- extensions, which run in their own sandboxed process (`wabcut-scripthost`) and must not get more than the
  permissions the user granted;
- the MCP server (`wabcut-cli mcp`), which is local only (stdio) and confined to the project's folder;
- AI provider keys, which must only ever live in the OS credential store, and the consent and no-cloud settings.

Out of scope: what an audio plug-in does once you load it (VST3 plug-ins are native code with your rights; Wabcut
runs them in a separate `wabcut-plughost` process so a crash cannot take the editor down, not as a security
boundary), vulnerabilities in third-party libraries that Wabcut has not exposed in a new way (please report those
upstream, though a heads-up is welcome), issues that need an attacker who already controls your Windows account,
and codec patent licensing.
