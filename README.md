# websocket-fft

A proof-of-concept for an RF FFT updated over Websockets, rendered using HTML5 Canvas.

librtlsdr -> libfftw3 -> json -> libwebsockets -> html5 canvas

![Browser Screenshot](http://i.imgur.com/4wW7KLh.png)

## Dependencies

* git submodule update --init
* sudo apt-get install libusb-1.0-0-dev libwebsockets-dev

### Known Issues

* Each unique line of FFT data appears to be sent to the browser twice.
* Occasional full-scale spike/notch artifacts are visible on the FFT rendering, restarting the server often fixes these.

### Credit

Projects used for reference included:

* libwebsockets test-server example [github.com/warmcat/libwebsockets/tree/v2.0-stable/test-server](https://github.com/warmcat/libwebsockets/tree/v2.0-stable/test-server)
* rtlizer [github.com/csete/rtlizer](https://github.com/csete/rtlizer)
* ccan (json lib) [github.com/rustyrussell/ccan](https://github.com/rustyrussell/ccan)
