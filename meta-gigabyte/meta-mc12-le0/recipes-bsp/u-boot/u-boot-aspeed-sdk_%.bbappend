require u-boot-common-aspeed-sdk.inc

# XXX FIXME HACK: replace ast2500-evb DTS because I cannot figure out how to place an _additional_ DTS/DTB and make that work properly
do_patch:append() {
# XXX FIXME why the hell is this inlined python?!
import subprocess
#subprocess.check_call('find . -name "*.dts" > /tmp/openbmc_dts_filetree', shell=True)
subprocess.check_call('cat ./sources/ast2500-gigabyte-mc12-le0.dts > ./sources/u-boot-aspeed-sdk-v2019.04+git/arch/arm/dts/ast2500-evb.dts', shell=True)
}

