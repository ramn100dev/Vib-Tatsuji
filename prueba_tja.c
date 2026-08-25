#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tja.h"

static tja_chart_t ch;

// Copia de calcular_puntos_nota() de motor.c. Es una copia a proposito y hay
// que tocarlas juntas: motor.c no se puede compilar aqui (mitad del fichero es
// PS2), y el reparto shin'uchi es justo lo que interesa comprobar contra
// partituras de verdad. Si esta prueba dice que una vuelta perfecta suma
// ~1.000.000, el reparto esta bien.
#define PUNTOS_TOTALES  1000000
#define PUNTOS_TRAMO        100

static int puntos_por_nota(const tja_chart_t *c, int *n_out, int *glo_out,
                           long long *rod_out)
{
	int i, n = 0, globos = 0;
	long long rod_ms = 0, resto;

	for (i = 0; i < c->n_notas; i++) {
		if (c->notas[i].tipo == TJA_DON || c->notas[i].tipo == TJA_KA) n++;
		else if (c->notas[i].tipo == TJA_GLOBO) globos += c->notas[i].golpes;
		else if (c->notas[i].tipo == TJA_RODILLO)
			rod_ms += c->notas[i].fin_ms - c->notas[i].tiempo_ms;
	}

	*n_out = n; *glo_out = globos; *rod_out = rod_ms;
	if (n == 0) return 0;

	resto = (long long)PUNTOS_TOTALES - (long long)globos * PUNTOS_TRAMO
	        - (1660LL * rod_ms) / 1000;
	if (resto < 0) resto = 0;

	return (int)(((resto + (long long)n * 10 - 1) / ((long long)n * 10)) * 10);
}

int main(int argc, char **argv)
{
	static const char *cursos[5] = {"0","1","2","3","4"};
	static const char *nom[5] = {"Easy","Normal","Hard","Oni","Edit"};
	const char *tn[4] = {"don","ka","RODILLO","GLOBO"};
	int a, k, i;

	for (a = 1; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb");
		long tam; char *buf;
		if (!f) { printf("no abre %s\n", argv[a]); continue; }
		fseek(f,0,SEEK_END); tam=ftell(f); fseek(f,0,SEEK_SET);
		buf = malloc(tam); fread(buf,1,tam,f); fclose(f);

		printf("=== %s\n", argv[a]);
		for (k = 0; k < 5; k++) {
			int r = tja_parsear(buf, tam, cursos[k], &ch);
			int nrod=0, nglo=0, ndon=0, nka=0, ngrande=0, malos=0;
			if (r != 0) continue;
			for (i = 0; i < ch.n_notas; i++) {
				switch (ch.notas[i].tipo) {
				case TJA_DON: ndon++; break;
				case TJA_KA:  nka++;  break;
				case TJA_RODILLO: nrod++; break;
				case TJA_GLOBO:   nglo++; break;
				}
				if (ch.notas[i].grande) ngrande++;
				if (ch.notas[i].fin_ms < ch.notas[i].tiempo_ms) malos++;
				if (i && ch.notas[i].tiempo_ms < ch.notas[i-1].tiempo_ms) malos++;
			}
			printf("  %-7s n=%-5d don=%-4d ka=%-4d gr=%-3d rod=%-3d glo=%-2d "
			       "dur=%-7d avisos_rod=%d sin_cuenta=%d desbord=%d %s\n",
			       nom[k], ch.n_notas, ndon, nka, ngrande, nrod, nglo,
			       ch.dur_ms, ch.avisos_rodillo, ch.globos_sin_cuenta,
			       ch.n_desbordadas, malos ? "  <-- ORDEN/FIN MAL" : "");
			{
				int gg = 0, ns = 0;
				float smin = 99.0f, smax = -99.0f;
				for (i = 0; i < ch.n_notas; i++) {
					if (ch.notas[i].gogo) gg++;
					if (ch.notas[i].scroll < smin) smin = ch.notas[i].scroll;
					if (ch.notas[i].scroll > smax) smax = ch.notas[i].scroll;
					if (i && ch.notas[i].scroll != ch.notas[i-1].scroll) ns++;
				}
				printf("       scroll: %d cambios, de %.4f a %.2f, min util "
				       "%.2f | gogo %d notas | complejos %d | ramas %d\n",
				       ns, smin, smax, ch.scroll_min, gg,
				       ch.avisos_scroll, ch.ramas_saltadas);
			}
			{
				int nn, glo; long long rod;
				int pn = puntos_por_nota(&ch, &nn, &glo, &rod);
				// Vuelta perfecta: todas las notas clavadas, todos los globos
				// reventados y el rodillo aporreado a los 16,6 golpes por
				// segundo que da por hecho el reparto.
				long long total = (long long)pn * nn
				                  + (long long)glo * PUNTOS_TRAMO
				                  + (1660LL * rod) / 1000;
				printf("       puntos: %d por nota x %d notas + %d globos + "
				       "%d ms rodillo = %lld %s\n",
				       pn, nn, glo, (int)rod, total,
				       (total >= 990000 && total <= 1010000)
				       ? "" : "  <-- FUERA DEL MILLON");
			}
			for (i = 0; i < ch.n_notas; i++)
				if (ch.notas[i].tipo >= TJA_RODILLO)
					printf("       %-8s %7d -> %-7d (%d ms)%s golpes=%d\n",
					       tn[ch.notas[i].tipo], ch.notas[i].tiempo_ms,
					       ch.notas[i].fin_ms,
					       ch.notas[i].fin_ms - ch.notas[i].tiempo_ms,
					       ch.notas[i].grande ? " GRANDE" : "",
					       ch.notas[i].golpes);
		}
		free(buf);
	}
	return 0;
}
