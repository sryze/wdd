dd for Windows
==============

[![Build Status][build_status]][build]

I made this little program for myself to back up USB flash drives and hard
disks on Windows. It has support for a few `dd` options such as `bs`, `count`
and `status=progress`.

Feel free to use it if you want. But I must warn you:

THIS IS AN EXPERIMENTAL PROGRAM. IT HAS NOT BEEN WELL TESTED AND IT CAN DAMAGE
YOUR DATA. USE AT YOUR OWN RISK.

Usage
-----

```
Usage: wdd if=<in_file> of=<out_file> [bs=N] [count=N] [status=progress]
```

`in_file` and `out_file` can be a file name or physical drive such as
`\\.\PHYSICALDRIVE0`. I haven't tested this with real hard disks, only flash
drives.

Example:

```
wdd if=\\.\physicaldrive3 of=usb.img bs=1M status=progress
```

To list available hard disks you can use this command:

```
wdd list
```

which internally executes:

```
wmic diskdrive list brief
```

[build]: https://ci.appveyor.com/project/sryze/wdd/branch/master
[build_status]: https://ci.appveyor.com/api/projects/status/2whky0cls6kwm840/branch/master?svg=true
