# Toolchain oficial: gcc MIPS + ps2sdk + gsKit + ps2-packer + ps2client ya compilados.
FROM ps2dev/ps2dev:latest

# xorriso hace falta para empaquetar el .elf en un .iso arrancable (OPL)
RUN apk add --no-cache bash make xorriso

WORKDIR /workspace
