# pb
## A command-line interface to macOS Pasteboard APIs

This is a command-line interface to the Pasteboard Manager, an obscure but modern API to the macOS Pasteboard system (which includes the clipboard and the Find pasteboard). Since its introduction in Mac OS X Panther, the Pasteboard Manager has supported multiple items and Uniform Type Identifiers, and pb exposes this functionality at the command line.

I wrote this tool in 2006 and 2009, with one additional commit in 2010. The code is pretty gnarly and could use some TLC, particularly as I struggled with getting some functionality working consistently. The fact that it's written in plain C—no Objective-C, not even Swift—didn't help. The prematurely-optimized style (in which I abhored any sort of centralized command-line argument processing) actively hindered.

That said, I'm tired of sitting on this thing and being the only one to benefit from it, so here it is.

### Usage

Most straightforwardly, `pb` with no arguments attempts to guess what you want and do that. If you pipe it into something, it will paste. If you pipe something else into it, it will copy. If you put it in the middle of a pipeline, it acts like `tee` where the secondary output is the general pasteboard. If it's not connected to a pipe at all, it will paste plain text.

`pb` also has a range of subcommands:

- `help` lists the subcommands.
- `list` lists the items that are on the pasteboard and the types/flavors that each item carries.
- `copy` reads from input and places the content on the pasteboard. By default, it assumes the input is plain text.
- `paste` takes the content from the pasteboard (by default, assuming it's plain text) and writes it to output.

If you pass a pathname to `copy` or `paste`, it will read or write that file rather than stdin/stdout.

If you pass a UTI, it will copy or paste that type rather than plain text.

If you pass `--item=NUM` to `paste`, it will paste item number `NUM` rather than the first item (item 0).
