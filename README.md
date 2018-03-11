dd for Windows
==============

I made this little program to back up USB flash drives and hard disks on
Windows. It has support for a few `dd` options such as `bs` and `count`. In
the future I plan to implement `status=progress`.

THIS IS AN EXPERIMENTAL PROGRAM, IT CAN DAMAGE YOUR DATA. USE WITH CAUTION.

Usage
-----

```sh
Usage: wdd if=<in_file> of=<out_file> [bs=N] [count=N] [status=progress]
```

`in_file` and `out_file` can be a file name or physical drive such as
`\\.\PHYSICALDRIVE0`. I haven't tested this with real hard disks, only flash
drives.

To list available physical drives you can use this command:

```sh
wmic diskdrive list brief
```
