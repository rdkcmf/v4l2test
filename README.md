# v4l2test

The v4l2test component is an application to test the v4l2 (Video For Linux V2) and dma-buf capabilities of a device.  The prerequisites to run this application on a candidate device are,
* v4l2 with VIDIOC_EXPBUF support,
* EGL with EGL_EXT_image_dma_buf_import support,
* GLES2, and
* support for DRM/KMS. 
 
If the GLES2 implementation includes GL_OES_EGL_image_external it will also be used for testing. 
The testing performs 1 to 4 concurrent video decodes.  All decode operations can use the same video stream, or each decode can use a distinct stream.

# Running

The app needs H264 video in a NAL byte-stream format.  This can be generated with gst-launch from a video accessible over http.  For example use the "Tears of Steel" video:

gst-launch-1.0 souphttpsrc location=http://ftp.nluug.nl/pub/graphics/blender/demo/movies/ToS/ToS-4k-1920.mov ! qtdemux ! h264parse ! "video/x-h264,stream-format=byte-stream" ! filesink location=/home/root/test.nal

The app tests performing 1 to 4 concurent video decodes.  All decode operations can use the same video stream, or each decode can use a distinct stream.  Each input is specified by a text descriptor file with the following format:

```
file: <nal stream filename>
frame-size: <width>x<height>
frame-rate: <fps>
```

For example:

```
file: /usb/TearsOfSteel.nal
frame-size: 1920x800
frame-rate: 24
```

The test has the following command line syntax:

```
v4l2test <options> <input-descr> [input-descr [input-descr [input_descr]]]
where
 input-descr is the name of a descriptor file with the format:
 file: <stream-file-name>
 frame-size: <width>x<height>
 frame-rate: <fps>

options are one of:
--report <reportfilename>
--devname <devname>
--window-size <width>x<height> (eg --window-size 640x480)
--numframes <n>
--verbose
-? : show usage
```

By default, the test will set a display resolution of 1080p (--window-size 1920x1080) and each decode will run for 400 frames (--numframes 400).  The test will auto-discover the v4l2 decoder device, but it can be manually specified with the --devname option.  The test will generate a report file at /tmp/v4l2test-report.txt, but this can be specified with the --report option.

To run a standard test use:

```
v4l2test stream1.txt stream2.txt stream3.txt stream4.txt
```

where streamN.txt are stream descriptor files.  The test will first perform a single decode displaying at fullscreen size, then a dual decode  with smaller side by side display size, then a triple decode, and finally a quad decode.

---
# Copyright and license

If not stated otherwise in this file or this component's Licenses.txt file the
following copyright and licenses apply:

Copyright 2019 RDK Management

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

