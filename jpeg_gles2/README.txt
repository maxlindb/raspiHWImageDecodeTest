How to use a Raspberry Pi to super-fast decoding of JPEG images and super-fast rendering using GLES2
by Matt Ownby, Oct 2013 (updated Apr 2015 with a bug fix for a race condition that shows up on the Pi 2)
--------------------------------------------------------------------------------------------------------

Greetings,

It's been a while (a year+) since my last tutorial ( http://www.raspberrypi.org/phpBB3/viewtopic.php?t=15463 ).

I've been wanting to release a follow-up to that for quite a while and I've finally had enough people inquiring about it to prompt me to do it.

This sample code demonstrates what I consider to be one of the most useful features of the Raspberry Pi:
  the ability to decode media using OpenMAX directly to a texture so that it can be rendered in arbitrary
   ways using GLES2.

Benefits of this approach:
- very good performance (almost completely hardware accelerated)
- flexible (you can render the resulting image in very arbitrary ways with GLES2)

As with my last sample, I provide benchmarking code so you can see how good performance is for you.
With the included .JPG, on an non-overclocked Pi, I am getting about 100 frames being decoded and rendered
every second.  This is about 66% faster than not decoding the JPEG directly to a GLES2 texture.  And it is
(dare I say it) exponentially faster than using libjpeg to decode JPEGs in software.

All of this code comes from my work in progress version of Daphne (http://www.daphne-emu.com) and so it
may appear to be overly designed for such a "simple" task.
I have tried to strip out as much unrelated code as I can,
but you may still see some references to Windows or Microsoft Visual Studio in a few files.
My apologies for that, but I think you will still be able to get the idea.

I have spent many MANY hours laboring over this code so I'd appreciate it if people would only use it for educational purposes.

Most of the code you are going to care about will be VideoObjectGLES2.cpp, VideoObjectGLES2_EGL.cpp,
 and JPEGOpenMax.cpp ; everything else is mainly just code to support those classes.

To build, just go change to the 'src' folder and type 'make' and
hopefully it will 'just work.'

Good luck!
 
--Matt Ownby
(http://www.daphne-emu.com, http://www.laserdisc-replacement.com, and http://my-cool-projects.blogspot.com)

-----------------------------------------------------------------
UPDATE: Benchmark on Raspberry Pi 2 (3 FPS faster after bug fix):

pi@raspberrypi /tmp/jpeg_gles2 $ ./jpeg_gles2 lair1.jpg
^CProperly shutting down...
Total elapsed milliseconds: 100949
Total frames displayed: 11350
Total frames / second is 112.433011
