require u-boot-common-aspeed-sdk.inc

# XXX FIXME HACK: replace ast2500-evb DTS because I cannot figure out how to place an _additional_ DTS/DTB and make that work properly
do_patch:append() {
# XXX FIXME why the hell is this inlined python?!
import subprocess
subprocess.check_call('cat ./sources-unpack/ast2500-gigabyte-mc12-le0.dts > ./git/arch/arm/dts/ast2500-evb.dts && git -C ./git/ diff > /tmp/ast2500_evb_diff', shell=True)
}

