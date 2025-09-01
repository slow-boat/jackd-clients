# jackmon
This is a jack client for monitoring jack sinks

- detects clipping
    * drive a GPIO - eg flash a LED
    * run a script
- can generate VU metering with RMS and peak hold
- has a VOX like level detect and hold function
	- connect sinks to source when triggered, disconnect when hold time expires
	- drive a GPIO- eg turn amplifiers on/off on signal
	- run a script 

Config file with notes is in [install directory](./install/etc/jackmon.conf)

## Build and install
- depends on pipewire-jack backend installed and running
- multi-instance systemd units to define each jackmon function

Build the binaries and install:

```
sudo apt install git build-essential libjack-jackd2-dev
git clone https://github.com/slow-boat/jackd-clients.git
cd jackd-clients/jackmon
make
sudo make install
```

### Create the instance
Lets call it *input*

```
sudo cp /etc/jackmon.conf /etc/jackmon.d/input.conf
```

Edit `/etc/jackmon.d/input.conf` as required

```
sudo systemctl daemon-reload
systemctl --user start jackmon@input.service
systemctl --user enable jackmon@input.service
```

This will run from boot as soon as pipewire is up.

# Examples
Using a **pi0w running raspbian**, we define three GPIOs, active high. This has two service units running:

- *input* unit switches analogue input to DSP/convolver input, and drives a gpio when its active to turn on an orange LED.
- *amp* unit simply turns a GPIO on and off.
- both units use a 3rd GPIO as a clipping indicator

GPIOS start at 512

- *clip* GPIO16 - pin36 - i=528
- *input_on* GPIO12 - pin32 - i=524 
- *amp_on* GPIO26 - pin37 - i=538

Go through build and install, then run this to customise and start:

```
cat << EOF > ~/input.conf
debug = 1
sources = Built-in Audio Stereo:capture_*
clip_gpio = 528
level_gpio = 524
level_sinks = Convolver Sink:playback_*
level_thres = -70.0
level_sec = 60
EOF

cat << EOF > ~/amp.conf
debug = 1
sources = Built-in Audio Stereo:monitor_*
clip_gpio = 528
level_gpio = 538
level_thres = -70.0
level_sec = 180
EOF
sudo mv ~/input.conf ~/amp.conf /etc/jackmon.d/
sudo systemctl daemon-reload
systemctl --user enable --now jackmon@input.service jackmon@amp.service
```

Using pretty vu `vu_pretty = 1` the console looks something like this:

```
|-80dB    |-60      |-40      |-20    0|
****************************|
************************  |
```

Note that each refresh redraws all but the first line. It resets the console top left, and clears/rewrites line 2 and 3 (..4 5 etc depending on number of channels.

