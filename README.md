- [how to run camera-gstreamer app](#how-to-run-camera-gstreamer-app)
  - [With PipeWire](#with-pipewire)
  - [With V4L2](#with-v4l2)
  - [Without a physical camera device](#without-a-physical-camera-device)
  - [Other Details](#other-details)
  - [cmd examples](#cmd-examples)

how to run camera-gstreamer app
===============================

With PipeWire
-------------
- PipeWire is the default path
- attach camera
	- attach a camera to Hardware.
	- make sure it is enumerated properly with `wpctl status` cmd
	- make the desired device node the default node with `wpctl set-default
	<camera-node-id>` cmd
- run
	- start camera-gstreamer from the UI.
	- The app can also be run from cmd prompt, make sure you login with `agl-driver` usr name.

With V4L2
---------
- login with `agl-driver` and start the app with `ENABLE_V4L2_PATH=true camera-gstreamer` cmd
- V4L2 path cannot be taken when the app is run from the UI.

Without a physical camera device
---------------------------------

The Virtual Video Test Driver (vivid) can be used for this purpose.

- run `modprobe vivid allocators=0x1` cmd
- check dmesg to know the capture device created.
- with PipeWire this device has to be made a default device, check [above](#with-pipewire) on how
  to do it.
- with V4L2 use DEFAULT_V4L2_DEVICE to pass this new device

Other Details
-------------
- For V4L2 path assumes that /dev/video0 is present and is set as a capture device.
  - Use DEFAULT_V4L2_DEVICE environmental variable to change it.
- use DEFAULT_DEVICE_WIDTH and DEFAULT_DEVICE_HEIGHT environmental variable to
override the default dimensions.

cmd examples
------------
run app with V4L2 path and on `/dev/video24`

```
DEFAULT_V4L2_DEVICE=/dev/video24 ENABLE_V4L2_PATH=true camera-gstreamer
```
run app with default path(PipeWire) with a video frame of height 480
```
DEFAULT_DEVICE_HEIGHT=480 camera-gstreamer
```

