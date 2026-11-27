hidapi 0.15.0 (https://github.com/libusb/hidapi), Linux hidraw and Windows backends.
Built into lh_usb (with hidden symbols on Linux: a host process may have another hidapi
loaded, such as monado-service's libusb backend). Local changes: thread-local global error
strings so parallel opens don't race; on Windows, hid_enumerate skips non-matching USB
VID/PID paths before opening handles, hid_open_path defers string-descriptor reads until
hid_get_device_info/hid_get_*_string so opening a device performs no USB control I/O, and
hid_close cancels pending overlapped reads with CancelIoEx before freeing buffers.
Licence: BSD (LICENSE-bsd.txt), as chosen in LICENSE.txt.
