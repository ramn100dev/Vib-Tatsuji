// Comprueba la conversion a Shift-JIS contra .tja de verdad.
//
// Saca, por cada TITLE/TITLEJA/SUBTITLE que encuentre, los bytes convertidos
// en hexadecimal. El script que lo llama los vuelve a descodificar con la
// tabla de Python y los compara con el original: si la ida y la vuelta
// cuadran, la conversion es correcta.
//
//   gcc -O1 -Wall -Wextra -I. -o /tmp/prueba_sjis prueba_sjis.c sjis.c sjis_tabla.c
//   /tmp/prueba_sjis "../Canciones prueba"/*/*.tja

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sjis.h"

static void una_linea(const char *clave, const char *valor)
{
	char conv[192];
	size_t i, n;

	a_sjis(conv, sizeof(conv), valor);
	n = strlen(conv);

	printf("%s\t%s\t", clave, valor);
	for (i = 0; i < n; i++) printf("%02x", (unsigned char)conv[i]);
	printf("\n");
}

int main(int argc, char **argv)
{
	int a;

	for (a = 1; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb");
		char linea[512];

		if (!f) { printf("no abre %s\n", argv[a]); continue; }

		while (fgets(linea, sizeof(linea), f)) {
			char *dp;

			linea[strcspn(linea, "\r\n")] = 0;
			// El BOM del principio, si lo hay
			if ((unsigned char)linea[0] == 0xEF) memmove(linea, linea + 3, strlen(linea) - 2);

			dp = strchr(linea, ':');
			if (!dp) continue;
			*dp = 0;
			if (strncmp(linea, "TITLE", 5) && strncmp(linea, "SUBTITLE", 8))
				continue;
			if (!dp[1]) continue;
			una_linea(linea, dp + 1);
		}
		fclose(f);
	}
	return 0;
}
