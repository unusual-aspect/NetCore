# NetClient library (compat)

Apps use `apps/NetClient/ClientSession` for the protocol client path.
This target is an INTERFACE alias of NetHandler so existing CMake link lines keep working.
