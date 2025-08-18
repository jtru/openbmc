# OpenBMC Gigabyte MC12-LE0 development

This is a community project to bring [OpenBMC](https://www.openbmc.org/) to the
[Gigabyte MC12-LE0](https://www.gigabyte.com/Enterprise/Server-Motherboard/MC12-LE0-rev-1x)
mainboard for AMD AM4-based Ryzen CPUs.

It is the result of a lot of work put into amateurish reverse-engineering of
this mainboard's official BMC firmware and some (mostly uneducated) guesswork,
and **not endorsed by GIGA-BYTE Technology Co., Ltd. or any of its
affiliates**.


## Disclaimer / Warranty

**This software is provided in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.**

Expect nothing to work, and everything to be broken. Some exceptions might
prove this rule.


## Installing

This comes first, because without the ability to actually try out build
artifacts on the proper hardware, spending time and compute on building the
images will probably be moot.

Since the MC12-LE0's BMC is powered by American Megatrends
[MegaRAC](https://en.wikipedia.org/wiki/MegaRAC) line of BMC firmware, it
differs significantly from OpenBMC. The stock firmware features a custom
fork/build of u-boot requiring a custom flash layout (i.e., partitioning
mechanism), a custom image format (so no booting from FIT images, and also no
Devicetree for a readable hardware description/configuration), and is based on
a heavily patched Linux kernel version 3.14, released 2014 AD, with a
proprietary BMC userland developed in-house by AMI.

To excise all this goodness from your mainboard and replace it with this broken
OpenBMC port, you can either use a hardware programmer to flash a complete
`image-bmc` ROM image onto the BMC SOIC16 SPI NOR flash located between the two
PCIe ports on the board, or follow a guide *that will be provided at a later
date* that details how to chain-load an intermediate build of u-boot from a
tftp server using the stock bootloader, that will then be able to boot into an
OpenBMC FIT image, also provided via tftp. With OpenBMC booted from the network
this way, flashing the actual MC12-LE0 OpenBMC ROM image from start to finish
(replacing the stock u-boot with OpenBMC's variant, too) can be performed quite
easily.


## Building

```sh
git clone --branch gigabyte-mc12-le0 https://github.com/jtru/openbmc
cd openbmc
. setup mc12-le0
time bitbake obmc-phosphor-image
```

Build artifacts will accumulate in `./tmp/deploy/images/mc12-le0/`, relative to
the working directory after sourcing the `setup` script.


## A Plea for Help

Going into this project, I had just about no experience at all with any of this
- not Yocto, not embedded Linux development, not any BMC internals, and I did
not even have enough experience with electronics in general to to be dangerous
to any free-roaming ICs on some PCB, anywhere.

As a result, and due to the (imho) rather high barrier of entry into OpenBMC
development without any kind of board schematics/documentation/vendor support,
lots of commits and even the current state of this branch is an utter mess.

Nevertheless I am *very* glad that I could even get this far: Most of the
kernel side of the work I believe/hope is actually done, and what's missing the
most is actually doing things *properly* (the way this branch integrates the
board's dts is horrible, for instance, and there's rough edges in just about
everything that I frantically copypasta'd from other bb layers I did not really
understand at the time, and do not fully grasp at present either) so that it
could maybe be properly upstreamed.

And that is not taking into account all the OpenBMC userland stuff to make the
software *actually* useful in operating a BMC, of course!

If you feel like helping out, please get a victim mainboard ready, make piece
with the idea of not using it for anything besides hacking on this project, and
get in touch.

Thanks! :-)


## What Should Work

With the big disclaimers above in mind, here's a list of what should work, if
you are, er, *adventurous* enough to flash this onto your mainboard:

With a resulting `image-bmc` flashed to the MC12-LE0, the board's BMC-exclusive
network port will be functional at 1Gbps link speed, and auto-configure IPv4
(DHCP) and IPv6 (SLAAC / DHCPv6) addresses.

SSH login to the BMC will work with default OpenBMC credentials, as well as
access via HTTPS to the [bmcweb](https://github.com/openbmc/bmcweb)-provided
web interface.

Power and reset control of the host works.

bmcweb's IP-KVM feature is functional.

System fan speed (and built-in speaker volume) can be controlled via PWM knobs
in OpenBMC's sysfs.

Some IPMI features (such as reading network configuration) from the host OS
work.

Accessing the host BIOS EEPROM works via its dedicated GPIO pin in certain host
power states.


## Acknowledgements

I would like to express my gratitude towards my dear Internet-friend Paul
Fertser, who helped me *so much* in getting this (very time-consuming project,
as it turned out ;-)) off the ground, and ever so patiently provided invaluable
help and kind, educational answers to my torrent of stupid questions. Thank
you, Paul!


Johannes Truschnigg <johannes@truschnigg.info> in August 2025
