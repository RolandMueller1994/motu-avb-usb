# motu-avb
Linux USB driver for the MOTU AVB series interfaces.
This version was tested with the Ultralite AVB ESS devices and tries to solve the problems with channel hopping and decimated sound.

**Main branch is to be used for kernel version >=6.5.0. If you're running kernel version <6.5.0, use branch pre_kernel_6.5**.

## Module parameters:

midi: Set to 1 for devices that have a midi port, 0 for the ones that don't

vendor: 0 = use class compliant mode (24 channels in/out), 1 = vendor mode (64/32/24)

channels: 0 = default (24 class compliant, 64/32/24 vendor mode), anything > 0 = number of channels in vendor mode. **Make sure to configure the device correctly before**.

time_silent: The time in milliseconds of silent data to be submitted at the start of the playback stream. The default is 2ms which complies with XHCI implementation that requires 2ms of queued data for continuous ISOC stream. In case you are still experiencing channel hopping or decimated sound, you might want to increase this value. Values lower than 2 might lead to dropouts in the ISOC stream but it will reduce latency.

frame_check: Default 0. If set to 1, the drivers tracks the start_frame of the playback urbs. In case the start_frame is further in the future than expected, the interface will be reset and the streams are restarted. **By default this option is disabled because it might not work on every XHCI controller but it is recommended to try using the option.**

Important: vendor mode requires to patch and recompile the kernel! Check [this](https://linuxmusicians.com/viewtopic.php?p=139957&sid=5dd8fd68d6b6abe5f40f5fffbb7faafc#p139957) post on linux musicians forum: 

it is recommended to set the parameters in the file /etc/modprobe.d/alsa-base.conf, e.g.

	options motu midi=1 vendor=0 frame_check=1

You may also make linux load the module during boot to prevent the alsa usb audio driver to take control of your device.
This is done by adding motu to file /etc/modules-load.d/modules.conf

Also, the device can be disabled in the alsa usb audio driver by adding:

	options snd_usb_audio enable=0 vid=0x07fd pid=0x0005 autoclock=no

to /etc/modprobe.d/alsa-base.conf.

## Preparations

In case you use a dual boot with windows, make sure you temporarily disable secure boot in the BIOS for installation
of the driver.

Install dkms and the kernel source of your running kernel, then

##Build

	sudo cp -r motu-avb-usb /usr/src/motu-avb-usb-1.3
	sudo dkms add motu-avb-usb/1.3
	sudo dkms build motu-avb-usb/1.3
	sudo dkms install motu-avb-usb/1.3

or run the install script

	sudo ./install.sh

If you want to use vendor mode, make sure you have curl installed, then connect the device through ethernet and execute the curl command

	curl  --data 'json={"value":"USB2"}' <ip address of the device>/datastore/host/mode
	
