# dataq
*Simple Linux client for [DATAQ DI-718B-E(S)](http://www.dataq.com/products/di-718b/) ethernet data acquisition system*

by Henry Hallam <henry@pericynthion.org>, 2016-06-07

Licensed under [CC0](https://creativecommons.org/publicdomain/zero/1.0/)
Or alternately under [MIT License](https://www.debian.org/legal/licenses/mit)

This should be portable to any POSIX system (e.g. OSX, Windows with Cygwin),
but it's only been tested on Linux.

Protocol reverse-engineered from WinDAQ via Wireshark, with help from
[UltimaSerial](http://www.ultimaserial.com/hack710.html) (thanks!)

Perhaps it will also work with the DI-718Bx 16-channel units or the DI-710 series?

## Dependencies
 none

## Build instructions
 `make`

If using as a standalone program, you'll probably want to edit the source
to set the scaling and the number of channels to poll.

To disable error messages, remove `-DEPRINT` in `Makefile`.

To disable debug messages, remove `-DDPRINT` in `Makefile`.

To use as a library (omit included `main()`), remove `-DUSE_MAIN` in `Makefile`,
define `main()` in a separate file that includes `dataq.h`, and link with `dataq.o`.

## Changes
2016-06-29 [MC] Refactored into functions, created header
