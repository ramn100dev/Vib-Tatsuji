#!/usr/bin/env python3
# Genera sjis_tabla.c: la tabla Unicode -> Shift-JIS de los caracteres que la
# fuente KROM de la BIOS de la PS2 sabe dibujar.
#
# Los rangos NO son inventados: son la tabla sjis_table[] de ps2sdk
# (ee/font/src/fontx.c), o sea exactamente los huecos que fontx_load carga del
# DOUBLE_BYTE de KROM. Generar la tabla desde ahi es lo que garantiza que no
# haya ninguna entrada para la que la consola no tenga glifo.
#
#   python3 gen_sjis.py > sjis_tabla.c

RANGOS = [
    (0x8140,0x817e),(0x8180,0x81ac),(0x81b8,0x81bf),(0x81c8,0x81ce),
    (0x81da,0x81e8),(0x81f0,0x81f7),(0x81fc,0x81fc),(0x824f,0x8258),
    (0x8260,0x8279),(0x8281,0x829a),(0x829f,0x82f1),(0x8340,0x837e),
    (0x8380,0x8396),(0x839f,0x83b6),(0x83bf,0x83d6),(0x8440,0x8460),
    (0x8470,0x847e),(0x8480,0x8491),(0x849f,0x84be),(0x889f,0x88fc),
    (0x8940,0x897e),(0x8980,0x89fc),(0x8a40,0x8a7e),(0x8a80,0x8afc),
    (0x8b40,0x8b7e),(0x8b80,0x8bfc),(0x8c40,0x8c7e),(0x8c80,0x8cfc),
    (0x8d40,0x8d7e),(0x8d80,0x8dfc),(0x8e40,0x8e7e),(0x8e80,0x8efc),
    (0x8f40,0x8f7e),(0x8f80,0x8ffc),(0x9040,0x907e),(0x9080,0x90fc),
    (0x9140,0x917e),(0x9180,0x91fc),(0x9240,0x927e),(0x9280,0x92fc),
    (0x9340,0x937e),(0x9380,0x93fc),(0x9440,0x947e),(0x9480,0x94fc),
    (0x9540,0x957e),(0x9580,0x95fc),(0x9640,0x967e),(0x9680,0x96fc),
    (0x9740,0x977e),(0x9780,0x97fc),(0x9840,0x9872),
]

pares = {}
for a, b in RANGOS:
    for sjis in range(a, b + 1):
        bs = bytes([sjis >> 8, sjis & 0xFF])
        try:
            u = bs.decode('cp932')
        except UnicodeDecodeError:
            continue
        if len(u) != 1:
            continue
        # Si dos codigos SJIS dan el mismo Unicode se queda el primero: el
        # segundo suele ser una duplicidad de cp932 fuera de los rangos utiles.
        pares.setdefault(ord(u), sjis)

items = sorted(pares.items())

print("// GENERADO POR gen_sjis.py -- NO EDITAR A MANO")
print("//")
print("// Unicode -> Shift-JIS, solo de los %d caracteres que KROM sabe" % len(items))
print("// dibujar: kana, simbolos y los kanji de JIS nivel 1. El nivel 2 no esta")
print("// en la fuente de la consola, asi que tampoco esta aqui.")
print("//")
print("// Ordenada por Unicode para poder buscar en binario. Cada entrada es")
print("// (unicode << 16) | sjis, que cabe de sobra en 32 bits: el Unicode mas")
print("// alto de la tabla es U+FFE5.")
print()
print("const unsigned int sjis_tabla[] = {")
for i in range(0, len(items), 6):
    fila = items[i:i+6]
    print("\t" + " ".join("0x%04X%04Xu," % (u, s) for u, s in fila))
print("};")
print()
print("const int sjis_tabla_n = %d;" % len(items))
print()
print("// Y los mismos rangos tal cual, que hacen falta APARTE: la cabecera del")
print("// font que se le pasa a libfont los lleva dentro (ver cargar_krom_kanji en")
print("// motor.c). Se sacan de aqui y no se copian a mano para que no puedan")
print("// separarse de la tabla de arriba.")
print("const unsigned short krom_rangos[] = {")
for i in range(0, len(RANGOS), 4):
    fila = RANGOS[i:i+4]
    print("\t" + " ".join("0x%04X, 0x%04X," % (a, b) for a, b in fila))
print("};")
print()
print("const int krom_rangos_n = %d;" % len(RANGOS))
