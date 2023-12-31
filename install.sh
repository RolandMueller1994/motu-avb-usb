#!/bin/bash

version="`grep -P '^PACKAGE_VERSION=(\d+\.\d+)$' -o dkms.conf | grep -P '\d+.\d+' -o`"
kernel_version="`uname -r`"

echo "${version}"
echo "${kernel_version}"

if [ $(dkms status -m motu-avb-usb -v ${version} -k ${kernel_version} | grep -c "motu-avb-usb") -eq 1 ]
then
	echo "Removing module version ${version} for kernel verison ${kernel_version}"
	dkms remove -m motu-avb-usb -v ${version} -k ${kernel_version}
fi

cp -r ../motu-avb-usb /usr/src/motu-avb-usb-${version}

dkms add motu-avb-usb/${version}
dkms build motu-avb-usb/${version}
dkms install motu-avb-usb/${version}

if [ $(lsmod | grep -c "motu") -eq 1 ]
then
	rmmod motu
fi

modprobe motu
