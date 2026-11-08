Placeholder for tagtype json files.
The needed json files are downloaded automatically from /resources/tagtypes in the Github repo.

This fork also ships its own BLE device types here (BC GICisky 2.9" BWRY, D0/D1/D2/DF Wolink,
E6 Nemonic MIP-201), so they are on the AP from the first boot and an AP whose repo setting still
points at upstream never downloads upstream's different D0..D2 (GFSK10 radio tags) instead.
Keep these files identical to /resources/tagtypes and raise "version" on every change: the update
page only replaces a local tagtype file when the repo has a higher version.
