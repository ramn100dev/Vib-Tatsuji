// Texto japones: UTF-8 -> Shift-JIS. Ver sjis.h.
//
// Esto es C portable a proposito, sin nada de PS2: asi se compila igual en el
// PC y la conversion se puede comprobar contra .tja de verdad, que es lo unico
// que demuestra que un titulo japones va a salir bien en la consola. Ver
// prueba_sjis.c.

#include <stdio.h>
#include <string.h>

#include "sjis.h"

// Devuelve el Shift-JIS de un punto Unicode, o 0 si KROM no lo sabe dibujar.
// Busqueda binaria: la tabla viene ordenada por Unicode desde el generador.
static unsigned int sjis_de_unicode(unsigned int u)
{
	int lo = 0, hi = sjis_tabla_n - 1;

	if (u > 0xFFFF) return 0;

	while (lo <= hi) {
		int med = (lo + hi) / 2;
		unsigned int k = sjis_tabla[med] >> 16;

		if (k == u) return sjis_tabla[med] & 0xFFFFu;
		if (k < u)  lo = med + 1;
		else        hi = med - 1;
	}
	return 0;
}

// Descodifica un caracter UTF-8. Devuelve cuantos bytes ocupaba, o 0 si la
// secuencia esta mal. Leer s[1] cuando s[0] es el ultimo byte es seguro: ahi
// esta el terminador, y (0 & 0xC0) != 0x80 lo rechaza.
static int utf8_siguiente(const unsigned char *s, unsigned int *cp)
{
	unsigned char c = s[0];

	if (c < 0x80) { *cp = c; return 1; }

	if ((c & 0xE0) == 0xC0) {
		if ((s[1] & 0xC0) != 0x80) return 0;
		*cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
		return (*cp >= 0x80) ? 2 : 0;          // sobrelargo: no vale
	}
	if ((c & 0xF0) == 0xE0) {
		if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
		*cp = ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
		return (*cp >= 0x800) ? 3 : 0;
	}
	if ((c & 0xF8) == 0xF0) {
		if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
		    (s[3] & 0xC0) != 0x80) return 0;
		*cp = 0x10000;      // fuera del plano basico: KROM no lo tiene igual
		return 4;
	}
	return 0;
}

static int es_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	unsigned int cp;

	while (*p) {
		int l = utf8_siguiente(p, &cp);
		if (l == 0) return 0;
		p += l;
	}
	return 1;
}

// UTF-8 -> Shift-JIS. Si la cadena NO es UTF-8 valido se copia tal cual: los
// .tja antiguos vienen en Shift-JIS de fabrica, que es justo lo que quiere
// fontx_print_sjis. Una cadena de solo ASCII es UTF-8 valido y se convierte en
// si misma, asi que este camino vale para todo.
//
// La deteccion no es perfecta y conviene saber por donde falla. Un kanji en
// Shift-JIS con cabecera 0xE0-0xEF y dos bytes de cola en 0x80-0xBF forma una
// secuencia UTF-8 valida, asi que se leeria mal. Pero esas cabeceras son las
// del nivel 2 de JIS, y basta con que en toda la cadena haya UN caracter de
// cabecera 0x81-0x9F (todo el nivel 1, todo el kana) para que la validacion
// falle y se tome el camino bueno. O sea que para colarse haria falta un
// titulo en Shift-JIS hecho SOLO de kanji del nivel 2, que ademas KROM no
// sabe dibujar. Se acepta el riesgo a cambio de no tener heuristicas que
// puedan equivocarse en el caso normal.
void a_sjis(char *dst, size_t n, const char *src)
{
	const unsigned char *p = (const unsigned char *)src;
	size_t o = 0;

	if (n == 0) return;

	if (!es_utf8(src)) {
		snprintf(dst, n, "%s", src);
		return;
	}

	// o + 3 < n deja sitio para los dos bytes de un kanji y el terminador.
	while (*p && o + 3 < n) {
		unsigned int cp, sj;
		int l = utf8_siguiente(p, &cp);

		if (l == 0) break;
		p += l;

		if (cp < 0x80) { dst[o++] = (char)cp; continue; }

		sj = sjis_de_unicode(cp);
		if (sj == 0) { dst[o++] = '?'; continue; }   // KROM no lo tiene

		dst[o++] = (char)(sj >> 8);
		dst[o++] = (char)(sj & 0xFF);
	}
	dst[o] = '\0';
}

// Recorta una cadena Shift-JIS a "max" bytes SIN partir un caracter de dos por
// la mitad. Partirlo deja un byte suelto que fontx_print_sjis toma por cabecera
// de kanji, y entonces se come el terminador y sigue leyendo.
//
// Recortar por BYTES y no por caracteres es ademas lo que hace que las medidas
// de siempre sigan valiendo: un kanji ocupa dos bytes y se dibuja al doble de
// ancho que una letra, asi que el ancho en pixeles sale igual.
void recorte_sjis(char *dst, size_t n, const char *src, int max)
{
	const unsigned char *p = (const unsigned char *)src;
	size_t o = 0;

	if (n == 0) return;
	if ((size_t)max > n - 1) max = (int)(n - 1);

	while (*p && (int)o < max) {
		int doble = ((*p >= 0x81 && *p <= 0x9F) || (*p >= 0xE0 && *p <= 0xEF));

		if (doble) {
			if (p[1] == '\0' || (int)o + 2 > max) break;
			dst[o++] = (char)*p++;
		}
		dst[o++] = (char)*p++;
	}
	dst[o] = '\0';
}
