# jackd-clients
Audio clients to connect to jackd2 server, or pipewire

## jackmon
This guy monitors a set of inputs specified by a string regex, and automatically connects to the specified jack sources.

### why?
- I need to turn amplifiers on and off to save power
- I need to connect analogue input ports to the DSP chain when a signal appears, and disconnect when the signal goes down
	- this is useful for muting noise from the ADCs when they are not in use and we are using a digital source- like snapclient.
- I want a simple way to get VU numbers to display on an external lcd/led array, and to get indication of clipping.
 
### basic details:

Depending on what you want it to do it can

- detect clipping (run a script or print a message)
- do VU metering to stdout or a file- with rms and peak pairs per channel
- threshold trigger with hold period: when the rms level exceeds specified threshold, turn a GPIO on, or run a script. The hold period timer is reset whenever the threshold is exceeded. If levels fall below the threshold for more than the period, the GPIO is turned off, or script called with env set to 0.
- threshold can also connect the sources to specified sinks. For example when an analogue input senses a sound signal, it can connect it to DACs or DSP sinks in the jack connection list.

## status

its very WIP but seems to be working OK in a pi0W with raspbian and pipewire, as well as my mint mate desktop machine.

- need to fix GPIO to use libgpiod instead of deprecated sysfs
- need to fix the VU meter to not use accumulator with floats since the errors get a bit too much.
- use a IIR LPF to average squared samples- creating a filtered mean squared value which should replicate RMS with VU meter like timing.
- create a socket or something more appropriate to be able to stream VU updates to another utility to push to the display.

