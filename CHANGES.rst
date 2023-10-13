This is a running log of changes to libvmod-vsthrottle.

libvmod-vsthrottle 1.0.2 (2023-10-13)
-------------------------------------

* Add check for clock_gettime()
* Clean up headers

Bugs fixed:
* [Issue #10] Fixed building on MacOS X Yosemite.

libvmod-vsthrottle 1.0.1 (2015-12-14)
-------------------------------------

* Ported to Varnish Cache 4.1.

Bugs fixed:
* [Issue #7] A token bucket is now also identified by its (period, limit) parameters.

libvmod-vsthrottle 1.0 (2014-04-24)
------------------------------------

* First public release.

libvmod-vsthrottle allows for rate-limiting traffic on a single Varnish server.
Offers a simple interface for throttling traffic on a per-key basis to a
specific request rate.

