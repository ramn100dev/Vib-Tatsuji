//=====================================================================
// tja.c - Lector de .tja
//
// El formato, en corto: la partitura son lineas de digitos donde cada
// digito es una subdivision del compas, y la COMA cierra el compas. O
// sea que "1001001," es un compas partido en 7, con notas en la 1, la 4
// y la 7. Cuantas subdivisiones tiene un compas lo decide cuantos
// digitos le hayas puesto, no hay un numero fijo.
//
// De ahi salen las tres cosas que hay que llevar bien:
//
//   1. Un compas dura (60000/bpm) * 4 * num/den ms, donde num/den es el
//      compas que diga el ultimo #MEASURE. splice.tja usa 4/4, 7/4, 7/8
//      y 8/4, y el primer #MEASURE esta en la linea siguiente a #START:
//      si se ignora, la cancion entera se desplaza desde la nota uno.
//   2. #BPMCHANGE cambia el BPM a mitad de partitura (4 veces por curso
//      en splice.tja), asi que el tiempo hay que ir acumulandolo compas
//      a compas con el BPM vigente. No vale multiplicar.
//   3. Un compas puede cruzar varias lineas, y entre medias puede haber
//      lineas de comando. En splice.tja hay un "100100" sin coma
//      seguido de un #GOGOSTART y el compas sigue despues. Por eso el
//      buffer se acumula entre lineas y solo lo cierra la coma.
//
// Sobre comandos de tiempo a mitad de compas: si un #BPMCHANGE cayera
// con el buffer a medias, las notas de antes y las de despues de ese
// compas duran distinto y esto lo calcularia mal. En los 5 cursos de
// splice.tja no pasa ni una vez (comprobado), asi que se aplica el
// comando y punto, pero se cuenta en "avisos_tiempo" para que no pase
// desapercibido con otro fichero.
//=====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tja.h"

//---------------------------------------------------------------------
// Utilidades de texto. Se hacen a mano porque strcasecmp y strtok no
// van a jugar bien con un buffer que no acaba en cero.
//---------------------------------------------------------------------
static int minus(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int igual_sin_mayus(const char *a, const char *b)
{
	while (*a && *b) {
		if (minus((unsigned char)*a) != minus((unsigned char)*b)) return 0;
		a++; b++;
	}
	return *a == 0 && *b == 0;
}

// Copia quitando espacios de los extremos.
static void copiar_recortado(char *dst, int max, const char *ini, const char *fin)
{
	int n;

	while (ini < fin && (*ini == ' ' || *ini == '\t')) ini++;
	while (fin > ini && (fin[-1] == ' ' || fin[-1] == '\t' ||
	                     fin[-1] == '\r' || fin[-1] == '\n')) fin--;

	n = (int)(fin - ini);
	if (n > max - 1) n = max - 1;
	if (n > 0) memcpy(dst, ini, (size_t)n);
	dst[n] = 0;
}

// "Easy" -> 0 ... "Edit" -> 4. Devuelve -1 si no lo reconoce.
static int numero_de_curso(const char *s)
{
	static const char *nombres[] = { "Easy", "Normal", "Hard", "Oni", "Edit" };
	int i;

	for (i = 0; i < 5; i++)
		if (igual_sin_mayus(s, nombres[i])) return i;

	// Tambien se admite el numero directamente
	if (s[0] >= '0' && s[0] <= '4' && s[1] == 0) return s[0] - '0';

	// "Ura" y "Edit" son lo mismo en muchos ficheros
	if (igual_sin_mayus(s, "Ura")) return 4;

	return -1;
}

//---------------------------------------------------------------------
int tja_parsear(const char *datos, long tam, const char *curso_pedido,
                tja_chart_t *out)
{
	const char *p, *fin_todo;
	int curso_buscado;
	int en_curso = 0, en_chart = 0, encontrado = 0;

	// Estado de la partitura
	double t_ms = 0.0;        // instante del compas actual
	double bpm = 120.0;
	int num = 4, den = 4;
	char compas[512];
	// El scroll y el gogo que llevaba cada digito. Van en paralelo a compas[]
	// y no en una variable suelta porque un #SCROLL puede caer en mitad de un
	// compas: las notas de antes van a una velocidad y las de despues a otra.
	float compas_scroll[512];
	char  compas_gogo[512];
	int n_compas = 0;

	double scroll = 1.0;
	int    gogo = 0;
	// Partituras bifurcadas: 0 = leyendo normal, 1 = saltandose una seccion.
	// Ver el bloque de #BRANCHSTART mas abajo.
	int en_rama = 0, saltando_rama = 0;

	// Globos. BALLOON: da los golpes de cada uno, en el orden en que salen
	// en la chart. Puede venir en la cabecera (vale para todos los cursos) o
	// dentro de un curso (pisa a la de cabecera). En los ficheros de prueba
	// siempre viene dentro del curso.
	int globos[TJA_MAX_GLOBOS];
	int n_globos = 0;         // cuantas cuentas hay
	int globo_actual = 0;     // cuantas se han repartido ya
	int fuera_de_cursos = 1;  // todavia no se ha visto ningun COURSE:

	// Tramo abierto (rodillo o globo). -1 = ninguno.
	int tramo = -1;

	if (datos == NULL || out == NULL || tam <= 0) return -3;

	curso_buscado = numero_de_curso(curso_pedido);
	if (curso_buscado < 0) return -1;

	memset(out, 0, sizeof(*out));
	// Se arranca en 1: si la chart no trae ni un #SCROLL, la velocidad es la
	// normal y el motor no tiene que mirar mas lejos de lo de siempre.
	out->scroll_min = 1.0f;
	out->bpm = 120.0f;

	p = datos;
	fin_todo = datos + tam;

	// El fichero viene en UTF-8 con BOM (comprobado en splice.tja); si no
	// se salta, la primera clave no se reconoce.
	if (tam >= 3 && (unsigned char)p[0] == 0xEF &&
	    (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
		p += 3;

	while (p < fin_todo) {
		const char *ini = p, *fin, *dp;

		// Delimita la linea (el fichero es LF, pero se admite CRLF)
		fin = ini;
		while (fin < fin_todo && *fin != '\n') fin++;
		p = (fin < fin_todo) ? fin + 1 : fin_todo;
		if (fin > ini && fin[-1] == '\r') fin--;

		// Comentarios: en .tja empiezan por // y llegan al final de linea
		for (dp = ini; dp + 1 < fin; dp++)
			if (dp[0] == '/' && dp[1] == '/') { fin = dp; break; }

		while (ini < fin && (*ini == ' ' || *ini == '\t')) ini++;
		if (ini >= fin) continue;

		//--- Claves de cabecera: CLAVE:valor ---
		if (*ini != '#') {
			const char *dosp = ini;
			while (dosp < fin && *dosp != ':') dosp++;

			if (dosp < fin) {
				char clave[24], valor[96];
				copiar_recortado(clave, sizeof(clave), ini, dosp);
				copiar_recortado(valor, sizeof(valor), dosp + 1, fin);

				if (igual_sin_mayus(clave, "COURSE")) {
					// Se entra en el curso pedido y se sale al siguiente
					fuera_de_cursos = 0;
					en_curso = (numero_de_curso(valor) == curso_buscado);
					en_chart = 0;
					if (en_curso)
						copiar_recortado(out->curso, sizeof(out->curso),
						                 dosp + 1, fin);
					continue;
				}

				// Las claves de fuera de los cursos valen para todos; las
				// de dentro del curso pedido pisan a las globales.
				if (!en_chart) {
					if (igual_sin_mayus(clave, "TITLE"))
						copiar_recortado(out->titulo, sizeof(out->titulo),
						                 dosp + 1, fin);
					else if (igual_sin_mayus(clave, "WAVE"))
						copiar_recortado(out->wave, sizeof(out->wave),
						                 dosp + 1, fin);
					else if (igual_sin_mayus(clave, "BPM"))
						out->bpm = (float)atof(valor);
					else if (igual_sin_mayus(clave, "OFFSET"))
						out->offset = (float)atof(valor);
					else if (igual_sin_mayus(clave, "LEVEL") && en_curso)
						copiar_recortado(out->nivel, sizeof(out->nivel),
						                 dosp + 1, fin);
					// Solo se hace caso a la BALLOON: del curso pedido, o a
					// la de la cabecera si todavia no ha empezado ningun
					// curso. La de OTRO curso no vale: pisaria la buena.
					// Se lee del texto crudo y no de "valor" porque la lista
					// puede ser mas larga que ese buffer.
					else if (igual_sin_mayus(clave, "BALLOON") &&
					         (en_curso || fuera_de_cursos)) {
						const char *q = dosp + 1;
						n_globos = 0;
						globo_actual = 0;
						while (q < fin && n_globos < TJA_MAX_GLOBOS) {
							while (q < fin && (*q == ' ' || *q == '\t' ||
							                   *q == ',')) q++;
							if (q >= fin || *q < '0' || *q > '9') break;
							{
								int v = 0;
								while (q < fin && *q >= '0' && *q <= '9')
									v = v * 10 + (*q++ - '0');
								globos[n_globos++] = v;
							}
						}
					}
				}
				continue;
			}
			// Sin dos puntos: si estamos dentro de la chart es una linea
			// de notas, y se trata mas abajo.
		}

		//--- Comandos ---
		if (*ini == '#') {
			char cmd[24];
			const char *sp = ini;
			while (sp < fin && *sp != ' ' && *sp != '\t') sp++;
			copiar_recortado(cmd, sizeof(cmd), ini, sp);

			if (igual_sin_mayus(cmd, "#START")) {
				if (!en_curso) continue;
				en_chart = 1;
				encontrado = 1;
				t_ms = 0.0;
				bpm = out->bpm;
				num = 4; den = 4;
				n_compas = 0;
				scroll = 1.0;
				gogo = 0;
				en_rama = 0;
				saltando_rama = 0;
				continue;
			}
			if (!en_chart) continue;

			if (igual_sin_mayus(cmd, "#END")) {
				en_chart = 0;
				en_curso = 0;
				break;                 // ya tenemos lo que queriamos
			}
			if (igual_sin_mayus(cmd, "#MEASURE")) {
				char v[24];
				int a, b;
				copiar_recortado(v, sizeof(v), sp, fin);
				if (sscanf(v, "%d/%d", &a, &b) == 2 && a > 0 && b > 0) {
					if (n_compas > 0) out->avisos_tiempo++;
					num = a; den = b;
				}
				continue;
			}
			if (igual_sin_mayus(cmd, "#BPMCHANGE")) {
				char v[24];
				double nuevo;
				copiar_recortado(v, sizeof(v), sp, fin);
				nuevo = atof(v);
				if (nuevo > 0.0) {
					if (n_compas > 0) out->avisos_tiempo++;
					bpm = nuevo;
				}
				continue;
			}
			//--- Partituras bifurcadas ---
			//
			// #BRANCHSTART abre un tramo que viene escrito TRES veces: la
			// version normal (#N), la avanzada (#E) y la maestra (#M), y el
			// juego salta de una a otra segun como lo estes haciendo. Aqui no
			// se evalua la condicion: se lee la PRIMERA seccion que aparezca
			// (que es #N, el camino normal) y las otras se saltan enteras.
			//
			// Saltarlas es obligatorio, no una comodidad: las tres describen
			// los MISMOS compases. Sin esto se leerian las tres seguidas y la
			// cancion tendria el triple de notas, cada tanda desplazada un
			// tramo entera. Antes de esto pasaba exactamente eso.
			if (igual_sin_mayus(cmd, "#BRANCHSTART")) {
				en_rama = 1;
				saltando_rama = 0;
				continue;
			}
			if (igual_sin_mayus(cmd, "#BRANCHEND")) {
				en_rama = 0;
				saltando_rama = 0;
				continue;
			}
			if (en_rama && (igual_sin_mayus(cmd, "#N") ||
			                igual_sin_mayus(cmd, "#E") ||
			                igual_sin_mayus(cmd, "#M"))) {
				// La primera que llega se lee; a partir de la segunda, fuera.
				if (saltando_rama == 0 && en_rama == 1) {
					en_rama = 2;              // ya estamos leyendo una
					saltando_rama = 0;
				} else {
					saltando_rama = 1;
					out->ramas_saltadas++;
				}
				n_compas = 0;
				continue;
			}
			if (saltando_rama) continue;      // nada de la seccion descartada

			//--- Velocidad de las notas ---
			//
			// #SCROLL multiplica la distancia a la que se dibuja una nota. No
			// toca el ritmo: el juicio va por tiempo, asi que esto es solo
			// como se lee la partitura.
			if (igual_sin_mayus(cmd, "#SCROLL")) {
				char v[32];
				copiar_recortado(v, sizeof(v), sp, fin);

				// Valores complejos ("0+1i", "-0.4-0.8i"): son los efectos de
				// mover las notas en vertical o en diagonal, que aqui no se
				// hacen. Se ponen a velocidad normal en vez de coger la parte
				// real, porque un "0+1i" daria scroll 0 y la nota se quedaria
				// clavada en el juez desde el principio de la cancion.
				if (strchr(v, 'i') != NULL || strchr(v, 'I') != NULL) {
					scroll = 1.0;
					out->avisos_scroll++;
				} else {
					double nuevo = atof(v);

					// Acotado a mano: hay charts con valores absurdos, y un
					// scroll enorme manda la nota a mil pantallas de distancia
					// mientras uno diminuto la deja pegada al juez toda la
					// cancion.
					if (nuevo >  20.0) nuevo =  20.0;
					if (nuevo < -20.0) nuevo = -20.0;
					scroll = nuevo;
				}
				continue;
			}

			//--- Gogo Time ---
			// Con el reparto shin'uchi NO cambia la puntuacion (ahi el x1,2 no
			// existe). Se guarda solo para poder enseñarlo.
			if (igual_sin_mayus(cmd, "#GOGOSTART")) { gogo = 1; continue; }
			if (igual_sin_mayus(cmd, "#GOGOEND"))   { gogo = 0; continue; }

			//--- Pausa ---
			// #DELAY <segundos> mete un silencio: todo lo que venga detras se
			// retrasa. Va sobre t_ms, que es el reloj del compas.
			if (igual_sin_mayus(cmd, "#DELAY")) {
				char v[24];
				double seg;
				copiar_recortado(v, sizeof(v), sp, fin);
				seg = atof(v);
				if (seg > 0.0 && seg < 600.0) t_ms += seg * 1000.0;
				continue;
			}

			// #BARLINEON/OFF, #JPOSSCROLL, #SUDDEN, #HBSCROLL, #NMSCROLL...
			// son efectos de dibujo que este motor no hace.
			continue;
		}

		if (!en_chart) continue;
		if (saltando_rama) continue;   // seccion de rama descartada

		//--- Linea de notas ---
		for (dp = ini; dp < fin; dp++) {
			if (*dp >= '0' && *dp <= '9') {
				if (n_compas < (int)sizeof(compas)) {
					compas_scroll[n_compas] = (float)scroll;
					compas_gogo[n_compas]   = (char)gogo;
					compas[n_compas++] = *dp;
				}
				continue;
			}
			if (*dp != ',') continue;

			// Coma: se cierra el compas y se reparten sus notas
			{
				double dur = (60000.0 / bpm) * 4.0 * (double)num / (double)den;
				int n = n_compas > 0 ? n_compas : 1;
				int j;

				for (j = 0; j < n_compas; j++) {
					char c = compas[j];
					double t;
					int tipo, ms;

					if (c == '0') continue;
					if (c < '1' || c > '8') continue;

					// El OFFSET del fichero se resta: un OFFSET negativo
					// (una intro larga) retrasa toda la partitura.
					t = t_ms + dur * (double)j / (double)n
					    - (double)out->offset * 1000.0;
					ms = (int)(t + 0.5);

					//--- 8: cierra el tramo abierto ---
					if (c == '8') {
						if (tramo < 0) {
							// Un 8 suelto. Pasa en charts reales; se ignora,
							// pero se apunta.
							out->avisos_rodillo++;
							continue;
						}
						out->notas[tramo].fin_ms = ms;
						tramo = -1;
						continue;
					}

					if (out->n_notas >= TJA_MAX_NOTAS) {
						out->n_desbordadas++;
						continue;
					}

					//--- 5, 6, 7: abren tramo ---
					if (c >= '5') {
						// Apertura con otra ya abierta: se cierra la
						// anterior aqui mismo. Anidar tramos no significa
						// nada en taiko, y dejarlo pasar dejaria el primero
						// con fin_ms sin poner: una barra hasta el infinito
						// y una ventana de golpe que se traga todo lo que
						// quede de cancion.
						if (tramo >= 0) {
							out->notas[tramo].fin_ms = ms;
							out->avisos_rodillo++;
						}

						tipo = (c == '7') ? TJA_GLOBO : TJA_RODILLO;

						out->notas[out->n_notas].tiempo_ms = ms;
						out->notas[out->n_notas].fin_ms    = ms;  // hasta el 8
						out->notas[out->n_notas].tipo      = tipo;
						out->notas[out->n_notas].grande    = (c == '6');
						out->notas[out->n_notas].golpes    = 0;
						out->notas[out->n_notas].scroll    = compas_scroll[j];
						out->notas[out->n_notas].gogo      = compas_gogo[j];

						if (tipo == TJA_GLOBO) {
							if (globo_actual < n_globos &&
							    globos[globo_actual] > 0) {
								out->notas[out->n_notas].golpes =
									globos[globo_actual];
							} else {
								out->notas[out->n_notas].golpes =
									TJA_GOLPES_POR_DEFECTO;
								out->globos_sin_cuenta++;
							}
							globo_actual++;
						}

						tramo = out->n_notas;
						out->n_rodillos++;
						out->n_notas++;
						continue;
					}

					//--- 1 a 4: notas normales ---
					tipo = (c == '1' || c == '3') ? TJA_DON : TJA_KA;

					out->notas[out->n_notas].tiempo_ms = ms;
					out->notas[out->n_notas].fin_ms    = ms;
					out->notas[out->n_notas].tipo      = tipo;
					out->notas[out->n_notas].grande    = (c >= '3');
					out->notas[out->n_notas].scroll    = compas_scroll[j];
					out->notas[out->n_notas].gogo      = compas_gogo[j];
					out->notas[out->n_notas].golpes    = 0;
					out->n_notas++;
				}

				t_ms += dur;
				n_compas = 0;
			}
		}
	}

	if (!encontrado) return -2;

	// El |scroll| positivo mas pequeño de toda la chart. Con el, el motor sabe
	// con cuanta antelacion tiene que empezar a mirar notas: cuanto mas bajo
	// el scroll, mas lejos en el tiempo esta una nota que ya se ve en
	// pantalla. Los valores diminutos (hay charts con 0,003) se dejan fuera
	// con un suelo, o la ventana se iria a minutos.
	{
		int i;
		float m = 1.0f;

		for (i = 0; i < out->n_notas; i++) {
			float v = out->notas[i].scroll;
			if (v < 0.0f) v = -v;
			if (v >= 0.2f && v < m) m = v;
		}
		out->scroll_min = m;
	}

	// Tramo que llega vivo al #END (o al final del fichero): se cierra donde
	// se acabo la chart. Sin esto se quedaria con fin_ms == tiempo_ms, o sea
	// un rodillo de duracion cero que no se puede golpear.
	if (tramo >= 0) {
		int cierre = (int)(t_ms - (double)out->offset * 1000.0 + 0.5);
		if (cierre < out->notas[tramo].tiempo_ms)
			cierre = out->notas[tramo].tiempo_ms;
		out->notas[tramo].fin_ms = cierre;
		out->avisos_rodillo++;
	}

	// La duracion es la del ultimo instante que se juega, que con rodillos
	// no tiene por que ser el de la ultima nota emitida.
	{
		int i;
		for (i = 0; i < out->n_notas; i++) {
			if (out->notas[i].tiempo_ms > out->dur_ms)
				out->dur_ms = out->notas[i].tiempo_ms;
			if (out->notas[i].fin_ms > out->dur_ms)
				out->dur_ms = out->notas[i].fin_ms;
		}
	}

	return 0;
}

const char *tja_error(int codigo)
{
	switch (codigo) {
	case  0: return "ok";
	case -1: return "curso no encontrado";
	case -2: return "el curso no tiene #START";
	case -3: return "datos invalidos";
	default: return "error desconocido";
	}
}
