# Tori

Tori is a bare-bones CLI torrent client. It supports downloading magnet URI torrents to a specified
directory.
That's it.

## Usage

The `magnet-uri` must be provided in quotes ("").

```
$ tori <magnet-uri> <path>
```

![](Docs/Demo.gif)

## Installing

You can check out the [Releases](https://github.com/jakerieger/Tori/releases) page for platform
installers or build from source
yourself.
Tori uses [libtorrent](https://github.com/arvidn/libtorrent) and its DLL's must be in the
same path as the Tori
executable.

The installer will automatically add Tori to your system path if enabled. If you're building from
source,
you'll have to add it to your path manually.