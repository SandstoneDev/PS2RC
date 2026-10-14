# Security

PS2RC is a build tool and a runtime for a games console. It runs on your own PC and
your own PSP; it has no network service and no online component.

## Reporting a problem

Please report a security problem privately, not in a public issue:

- use the repository's "Report a vulnerability" button (GitHub private vulnerability
  reporting), or
- write to sandstonedevpsp@gmail.com.

Include the version (commit), what you found and how to reproduce it.

Examples of what to report:

- a crafted disc image or game pack that makes the translator or the build tools run
  code or write files outside their work directory on the PC;
- a way for a build to damage a PSP's flash or settings;
- secrets or personal data committed to the repository by mistake.

## Takedown requests

Requests from rightsholders are not security reports: see [TAKEDOWN.md](TAKEDOWN.md).
In short, contact the maintainer through GitHub issues, the community server or
sandstonedevpsp@gmail.com; named material is removed promptly and not published
again, and GitHub's DMCA process applies as usual.

## Scope notes

- The kernel-mode helper module on the PSP runs with kernel privileges on a console
  with custom firmware. Treat changes to it with extra care.
- Game packs are code: build only packs you trust, as you would with any source code
  you download.
- PS2RC does not handle copy protection and will not accept changes that circumvent
  it.
