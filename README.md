# rtkit-ng

`rtkit-ng` is a (work in progress) reimplementation of rtkit using modern kernel features and the
`sd-bus` dbus library.

## TODO

* [x] Process owner checks
* [ ] Policy Kit support
* [ ] Watchdog?
* [x] Per-User burst limits?
* [ ] Limits on how many processes might be able to set
* [ ] Installation of data files
* [ ] Privilege dropping
  * [ ] Dropping capabilities
  * [ ] Separate user? How to handle hidepid=2?
* [ ] Replace user/pid lists with AA-trees?

## Dependencies

* `sd-dbus`, provided by `libsystemd`, `libelogind` or `basu`.

## License

GPLv3+ for the daemon
