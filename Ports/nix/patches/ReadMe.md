# Patches for nix on SerenityOS

## `0001-Add-missing-sys-select.h-include.patch`

Add missing <sys/select.h> include

`select()` may not be ambiently available for use on every platform

Merged upstream post-`2.16.1` and will be included in the next release
https://github.com/NixOS/nix/pull/8456

## `0002-No-implementation-of-sys-queue.h-in-Serenity.patch`

No implementation of `<sys/queue.h>` in Serenity

Serenity currently doesn't have an implementation of `<sys/queue.h>`,
a dependency for lowdown. The lowdown CLI tool detects that `<sys/queue.h>`
isn't available during `configure` and adds a built-in implementation.
However lowdown doesn't include its compatibility shims when used
as a library via `lowdown.h`. Nix worked around this issue by adding
`#include <sys/queue.h>` before importing `lowdown.h`. We can hack
around this by just stubbing out uses of `<sys/queue.h>` functionality
till we have a Serenity equivalent. :)

