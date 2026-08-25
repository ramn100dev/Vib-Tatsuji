// Texto japones: paso de UTF-8 a Shift-JIS, que es lo que quiere
// fontx_print_sjis de ps2sdk.
//
// Esto es C portable a proposito, sin nada de PS2: asi se puede compilar en el
// PC y comprobar la conversion contra .tja de verdad (ver prueba_sjis.c). El
// resto del motor no se puede.
#ifndef SJIS_H
#define SJIS_H

#include <stddef.h>

// Tabla Unicode -> Shift-JIS de los caracteres que la fuente KROM de la BIOS
// sabe dibujar. La genera gen_sjis.py a partir de la tabla de rangos de
// ps2sdk, o sea que no puede tener entradas sin glifo.
extern const unsigned int sjis_tabla[];
extern const int          sjis_tabla_n;

// Los 51 rangos de Shift-JIS que cubre KROM, en pares (principio, fin). El
// motor los necesita aparte para montar la cabecera del font de kanji.
extern const unsigned short krom_rangos[];
extern const int            krom_rangos_n;

// UTF-8 -> Shift-JIS. Si la cadena no es UTF-8 valido se copia tal cual: los
// .tja antiguos vienen en Shift-JIS de fabrica.
void a_sjis(char *dst, size_t n, const char *src);

// Recorta a "max" bytes sin partir un caracter de dos bytes por la mitad.
void recorte_sjis(char *dst, size_t n, const char *src, int max);

#endif
