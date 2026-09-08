# oe-core's own default PACKAGECONFIG for btrfs-tools includes
# "python" (builds and installs libbtrfsutil's Python bindings) -
# found via real-world use: python3 unexpectedly ended up in this
# minimal installer image, traced back here. We only ever call the
# plain CLI "btrfs subvolume list" via subprocess (see backend.hpp's
# probe_is_rootfs()), never any Python bindings - hard override to
# drop "python" from the default set, keeping everything else
# (programs/convert/crypto-builtin) unchanged.
PACKAGECONFIG = "programs convert crypto-builtin"
