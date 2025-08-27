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

Initial configuration after building binary *jackmon* for the platform (this will get integrated into make with make install)
```
sudo cp -r install/* /
sudo cp jackmon /usr/sbin/
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