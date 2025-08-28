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

- detect clipping- drive a LED, or run a script
- do VU metering to stdout or a file- with rms and peak pairs per channel, or a basic console VU meter.
- threshold trigger with hold period: when the rms level exceeds specified threshold, turn a GPIO on, route sources to specified trigger sink ports, and/or run a script. The hold period timer is reset whenever the threshold is exceeded.
- see the config file for details.
- systemd unit scripts will be added, to allow multiple instances
    * for example instances to connect analogue input ports, or zita-n2j client to DSP chain sink, monitor clipping, and provide a LED to indicate connection.
    * another instance to do VU metering on the final output, and control amplifier via GPIO, with clipping LED.

