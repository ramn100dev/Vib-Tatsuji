# Motor de ritmo, paso 1: reloj de cancion.
#
# Todo va empotrado en el .elf (IRX, sonidos de golpe y una cancion de
# reserva) para que no dependa de host: como hacia Sounds/. Lo unico que
# se lee de fuera es el .ogg del pen, y si no hay pen tira del empotrado.

EE_BIN = motor.elf

EE_OBJS = motor.o tja.o sjis.o sjis_tabla.o \
          iomanX_irx.o fileXio_irx.o bdm_irx.o bdmfs_fatfs_irx.o \
          usbd_irx.o usbmass_bd_irx.o audsrv_irx.o \
          don_adp.o bigdon_adp.o cancel_adp.o \
          click140_ogg.o \
          libogg_fix/framing.o

EE_LIBS = -ldebug -lfont -ldraw -lgraph -lmath3d -lpacket -ldma -lpad -laudsrv \
          -lfileXio -lpatches -lvorbisfile -lvorbis -logg -lm

# ogg y vorbis viven en ports/, no en la ruta por defecto del sdk
EE_INCS := -I$(PS2SDK)/ports/include $(EE_INCS)
EE_LDFLAGS := -L$(PS2SDK)/ports/lib $(EE_LDFLAGS)

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)

# "make auto" saca el mismo .elf recorriendo el ciclo menu->partida->menu el
# solo, para poder probarlo en PCSX2 sin mando conectado.
auto:
	$(MAKE) clean
	$(MAKE) EXTRA="-DAUTOCICLO=1"
	mv $(EE_BIN) motor_auto.elf
	# El clean final NO es cosmetico: sin el, el motor.o de esta variante se
	# queda ahi y el siguiente "make" relinca el .elf normal CON el autociclo
	# dentro. Eso en una ISO que va a la consola son tres horas de no
	# entender por que la cancion se corta sola a los 30 segundos.
	$(MAKE) clean

# framing.c de libogg recompilado a -O1 a proposito.
#
# A -O2 gcc convierte el "leer 4 bytes y montarlos a mano" de
# ogg_page_serialno() en un par lwl/lwr, y el recompilador EE de PCSX2 no
# les extiende el signo: todo .ogg cuyo numero de serie tenga el bit 31 a
# 1 acaba dando -132 (OV_ENOTVORBIS). click140.ogg es uno de esos, con
# serial 0x96835D56, asi que este arreglo aqui NO es opcional.
# Detalle completo en DISENO.md.
libogg_fix/framing.o: libogg_fix/framing.c
	$(EE_CC) $(EE_CFLAGS) -O1 $(EE_INCS) -c $< -o $@
	@n=$$($(EE_TOOL_PREFIX)objdump -d $@ | grep -cE "	(lwl|lwr|ldl|ldr)[[:space:]]") ; \
	 if [ "$$n" != "0" ]; then \
	   echo "ERROR: $@ tiene $$n cargas no alineadas; volveria el -132." ; \
	   rm -f $@ ; exit 1 ; \
	 fi

# --- IRX empotrados ---
iomanX_irx.c: $(PS2SDK)/iop/irx/iomanX.irx
	bin2c $< iomanX_irx.c iomanX_irx
fileXio_irx.c: $(PS2SDK)/iop/irx/fileXio.irx
	bin2c $< fileXio_irx.c fileXio_irx
bdm_irx.c: $(PS2SDK)/iop/irx/bdm.irx
	bin2c $< bdm_irx.c bdm_irx
bdmfs_fatfs_irx.c: $(PS2SDK)/iop/irx/bdmfs_fatfs.irx
	bin2c $< bdmfs_fatfs_irx.c bdmfs_fatfs_irx
usbd_irx.c: $(PS2SDK)/iop/irx/usbd.irx
	bin2c $< usbd_irx.c usbd_irx
usbmass_bd_irx.c: $(PS2SDK)/iop/irx/usbmass_bd.irx
	bin2c $< usbmass_bd_irx.c usbmass_bd_irx
audsrv_irx.c: $(PS2SDK)/iop/irx/audsrv.irx
	bin2c $< audsrv_irx.c audsrv_irx

# --- Sonidos de golpe empotrados ---
%.adp: %.wav
	$(PS2SDK)/bin/adpenc $< $@

don_adp.c: Don.adp
	bin2c $< don_adp.c don_adp
bigdon_adp.c: BigDon.adp
	bin2c $< bigdon_adp.c bigdon_adp
cancel_adp.c: Cancel.adp
	bin2c $< cancel_adp.c cancel_adp

# --- Cancion de reserva ---
click140_ogg.c: click140.ogg
	bin2c $< click140_ogg.c click140_ogg

# --- Empaquetado en .iso para OPL ---
#
# OPL arranca desde el disco duro una imagen ISO9660 normal: lo unico que
# necesita es un SYSTEM.CNF en la raiz apuntando al .elf. Nivel 1 porque
# es el que usa nombres 8.3 con la version ";1" que espera el BOOT2.
# xorriso vive dentro del contenedor de ps2dev (ver Dockerfile).
iso: all
	cp $(EE_BIN) iso_root/MOTOR.ELF
	xorriso -as mkisofs -iso-level 1 -V MOTOR -sysid PLAYSTATION \
	        -o motor.iso iso_root/

clean:
	rm -f $(EE_BIN) motor_auto.elf motor.iso iso_root/MOTOR.ELF \
	      $(EE_OBJS) *_irx.c *_adp.c click140_ogg.c *.adp

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

# Va DESPUES de los includes: Makefile.pref asigna EE_CFLAGS con "=", asi que
# puesto antes se perderian -D_EE, -G0 y compania.
EE_CFLAGS += $(EXTRA)
