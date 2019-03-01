# cncstream

An implementation of GRBL's `stream.py` in C. It is probably buggy.

    make
    ./cncstream -d /dev/ttyACM0 -f test.gcode

The goal is to make a daemon that will run continuously and respond to protobuf
messages to queue a job and monitor it's progress. Nice features would be:

* Listen on a socket (local) for API messages to queue a GCode file to be
  streamed or get the status of a stream.
* Be udev aware so it can respond to new GRBL devices being plugged in.
* Allow multiple GRBL devices to be running at the same time doing different
  things.
