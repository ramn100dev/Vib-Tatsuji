//=====================================================================
// tja.h - Lector de ficheros .tja (las partituras de open-taiko).
//
// Saca los cuatro tipos que usa el juego:
//
//   1 don, 2 ka          notas normales
//   3 DON, 4 KA          grandes (marcadas con "grande")
//   5 rodillo, 6 RODILLO grande, 7 globo    -> abren un tramo
//   8                    cierra el tramo abierto
//
// Los tramos (rodillo y globo) salen como UNA entrada con tiempo_ms de
// inicio y fin_ms de final. En las notas normales fin_ms == tiempo_ms, para
// que el motor pueda tratarlas todas igual sin mirar el tipo.
//=====================================================================
#ifndef TJA_H
#define TJA_H

// 8192 y no 4096: el curso mas cargado de las canciones de prueba se queda en
// 1603, pero un Oni largo de verdad pasa de 4000 y el desbordamiento se come
// las notas del final sin que se note hasta que llegas ahi.
#define TJA_MAX_NOTAS  8192

#define TJA_DON      0
#define TJA_KA       1
#define TJA_RODILLO  2   // 5 y 6
#define TJA_GLOBO    3   // 7

// Cuentas de golpe de los globos (clave BALLOON:). De sobra: la chart mas
// cargada de las de prueba tiene 4.
#define TJA_MAX_GLOBOS  64

// Globos sin cuenta en BALLOON:. Un 0 seria peor que cualquier numero: se
// reventaria solo en el primer frame.
#define TJA_GOLPES_POR_DEFECTO  5

typedef struct {
	int tiempo_ms;   // ya con el OFFSET del fichero aplicado
	int fin_ms;      // rodillos y globos: final del tramo. Si no, == tiempo_ms
	int tipo;        // TJA_DON / TJA_KA / TJA_RODILLO / TJA_GLOBO
	int grande;      // 1 si venia como 3, 4 o 6
	int golpes;      // globos: cuantos golpes hacen falta
	// Multiplicador de velocidad que llevaba puesto el #SCROLL cuando se
	// escribio esta nota. Es POR NOTA y no del compas: un #SCROLL puede caer
	// en mitad de un compas y afectar solo a las notas que van detras.
	float scroll;
	int   gogo;      // 1 si cae dentro de un tramo de Gogo Time
} tja_nota_t;

typedef struct {
	char  titulo[64];
	char  wave[64];      // nombre del .ogg que pide la partitura
	float bpm;           // el BPM de cabecera (puede cambiar con #BPMCHANGE)
	float offset;        // segundos, tal cual viene en el fichero
	char  curso[16];
	char  nivel[8];

	tja_nota_t notas[TJA_MAX_NOTAS];
	int n_notas;
	int dur_ms;          // instante de la ultima nota

	int n_rodillos;      // rodillos y globos emitidos
	int n_desbordadas;   // notas que no cupieron en TJA_MAX_NOTAS
	int avisos_tiempo;   // #BPMCHANGE/#MEASURE a mitad de compas (ver tja.c)
	// Tramos mal formados: un 8 sin nada abierto, una apertura encima de
	// otra abierta, o un tramo que llega vivo al #END. Pasan en charts
	// reales y no son fatales, pero conviene que se vean.
	int avisos_rodillo;
	int globos_sin_cuenta;   // globos que no tenian numero en BALLOON:
	// #SCROLL con valor complejo ("0+1i"): son efectos de movimiento que aqui
	// no se hacen, asi que esas notas van a velocidad normal.
	int avisos_scroll;
	// Secciones de partitura bifurcada (#BRANCHSTART) que se han saltado.
	int ramas_saltadas;
	// El |scroll| positivo mas pequeño de la chart. El motor lo necesita para
	// saber con cuanta antelacion tiene que empezar a mirar notas: con un
	// scroll bajo, una nota lejanisima ya se ve en pantalla.
	float scroll_min;
} tja_chart_t;

// Devuelve 0 si va bien, negativo si no.
//   -1 curso no encontrado   -2 sin #START   -3 datos invalidos
// "curso" acepta nombre ("Easy") o numero ("0"), sin distinguir mayusculas.
int tja_parsear(const char *datos, long tam, const char *curso,
                tja_chart_t *out);

const char *tja_error(int codigo);

#endif
