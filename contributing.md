# Contributing
TLDR: Follow general FreeBSD conventions.

Open an issue before writing anything substantial. If a change touches something in design.md, say which decision it affects, some behaviour that looks like a bug is a decision with a rationale behind it, and some of it is genuinely wrong.

## Style
Code follows [style(9)](https://man.freebsd.org/style/9)

Markdown paragraphs stay on one line; GitHub and Obsidian reflow on render, so hard-wrapping only makes diffs noisier.

## Testing
Build and test on a kernel with `WITNESS` and `INVARIANTS`. Lock-order and assertion problems in this driver are usually invisible without them.

Every behavioural change needs a case in `tests/veb_test.sh`. If something is genuinely untestable from a shell, say so in the commit message and explain how you verified it instead. `design.md` records which decisions have coverage; new gaps should not appear silently.

## Locking
Lock order is `VEB_LOCK` -> `VPORT_LOCK` and `VEB_LOCK` -> `VEB_RT_LOCK`. Neither reverse is taken, and `vport_ioctl()` must never acquire `VEB_LOCK`. A change that needs a new lock or a new order is a design change, raise it first.

## Commits
One logical change per commit, in FreeBSD's format: a short subject prefixed with the component, then a body explaining why rather than what.

If the change alters a documented decision, cite the ID (`D3`, `D7`, ...) and update `design.md` in the same commit.

## License
By contributing you agree that your work is licensed under BSD-3-Clause, matching the rest of the repository.
