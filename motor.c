//=====================================================================
// motor.c - El motor: menu, partida y vuelta al menu.
//
// El programa es un ciclo:
//
//   menu()      dos pantallas: lista de canciones y dificultad
//   render()    la partida, y de ahi a la pantalla de resultados
//               -> y vuelta al menu
//
// Todo se maneja con el tambor y solo con el tambor: los dos bordes azules
// mueven y los dos parches rojos eligen. Un tambor no tiene cruceta, asi que
// ningun menu puede pedir cuatro direcciones; de ahi que la dificultad este en
// pantalla aparte. START abre la pausa durante la cancion (pantalla_pausa) y
// no hace nada en el menu.
//
// Entre una cancion y la siguiente NO se toca audsrv: solo se rehace el
// hilo de audio. Todo lo que hay que dejar como estaba vive junto en
// preparar_cancion(), porque cada cosa que se olvide ahi rompe la SEGUNDA
// cancion de una forma distinta y ninguna se parece a un fallo de audio.
//
//---------------------------------------------------------------------
// LO QUE YA ESTABA: EL RELOJ DE CANCION
//
// Junta lo de Sounds/ (GS, mando, sonidos ADPCM) con lo de Audio_Ogg/
// (USB + libvorbisfile + audsrv), pero cambiando lo mas importante:
//
//   ANTES (Sounds):  las notas se movian "cx -= velocidad" cada frame
//                    y el acierto se juzgaba por DISTANCIA EN PIXELES.
//   AHORA:           las notas tienen un instante en MILISEGUNDOS, se
//                    dibujan a partir de la posicion real de la cancion
//                    y el acierto se juzga por DIFERENCIA DE TIEMPO.
//
// La diferencia no es cosmetica: con el metodo viejo, en cuanto un frame
// se pasa de vsync la musica y las notas se separan y no vuelven a
// juntarse jamas. Con el reloj de audio, un frame perdido solo hace que
// las notas den un salto, pero siguen cuadrando con la musica.
//
//---------------------------------------------------------------------
// COMO SE SACA LA POSICION DE LA CANCION
//
// audsrv no tiene ninguna funcion que diga "vas por el segundo 12" para
// audio en streaming (audsrv_get_trackpos es solo para CD). Lo que si
// tiene es audsrv_queued(): cuantos bytes hemos mandado que todavia no
// han sonado. Con eso:
//
//   posicion_ms = (bytes_enviados - audsrv_queued()) * 1000 / bytes_por_seg
//
// Eso da un valor correcto pero que solo se refresca cada trozo (~10 ms)
// y ademas lleva un desfase constante, porque el DMA al SPU2 va por
// delante de la cola. El desfase constante da igual: se absorbe en el
// OFFSET_LATENCIA_MS que se calibra una vez. Lo que no da igual es el
// escalon, asi que entre trozo y trozo se interpola con el contador de
// ciclos del EE (cpu_ticks).
//
//---------------------------------------------------------------------
// POR QUE HAY UN HILO DE AUDIO Y POR QUE EL SOLO TOCA audsrv
//
// Decodificar Vorbis cuesta ~34% del EE a 48 kHz estereo, asi que no
// puede ir en el bucle de dibujo o se comeria el frame. Va en su hilo.
//
// Pero entonces habria dos hilos llamando a audsrv, y todas las llamadas
// de audsrv van por SIF RPC compartiendo la misma estructura de cliente:
// dos hilos a la vez ahi corrompen el estado de la llamada en vuelo.
// Solucion sin candados: el hilo de audio es el UNICO que llama a
// audsrv. El bucle de dibujo, cuando quiere un sonido de golpe, solo
// incrementa un contador; el hilo de audio lo ve y lo reproduce el.
// Por eso los trozos son de 2048 bytes (~10 ms a 48k estereo) y no de
// 8192 (~42 ms): con trozos grandes el sonido del golpe llegaria tarde.
//=====================================================================

#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <tamtypes.h>
#include <timer.h>

#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <debug.h>

#include <libpad.h>
#include <audsrv.h>

#include <packet.h>
#include <dma_tags.h>
#include <gif_tags.h>
#include <gs_psm.h>
#include <dma.h>
#include <graph.h>
#include <gs_privileged.h>
#include <draw.h>
#include <draw3d.h>
#include <font.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>
#include <iox_stat.h>

#include <vorbis/vorbisfile.h>

#include "tja.h"
#include "sjis.h"

//---------------------------------------------------------------------
// Ajustes
//---------------------------------------------------------------------

// Trozo de PCM que se manda a audsrv de una vez. Manda la latencia con
// la que responden los sonidos de golpe, ver cabecera.
#define TROZO_PCM        2048

// Cuanto se retrasa el sonido respecto a la imagen; positivo = la musica va
// por detras de lo que crees. Esto ya solo es el valor de partida: el que se
// usa vive en offset_ms, sale del fichero de ajustes del pen y se mide con el
// metronomo (ver "Calibracion" mas abajo).
#define OFFSET_LATENCIA_MS  0

// Fichero de ajustes, en la raiz del pen. Nombre 8.3 y en mayusculas a
// proposito: es el unico que se escribe, y con FAT no hay que confiar en que
// el nombre largo sobreviva.
#define RUTA_CONFIG  "mass0:/TATSUJI.CFG"
// Da de sobra para las claves de ahora y para unas cuantas mas. Si algun dia
// no cabe, lo que pasa es que las ultimas claves no se leen: por eso el offset
// se escribe el primero.
#define TAM_CONFIG   256

// Suavizado del reloj. La medida bruta que sale de audsrv_queued() tiene
// un diente de sierra de +-5 ms y, de vez en cuando, un valor absurdo
// (ver hilo_audio). En vez de creerse cada medida, el reloj corre solo
// con el contador de ciclos y se corrige como mucho ARRASTRE_MS por
// trozo, que a ~93 trozos por segundo dan 93 ms/s de margen: mas que de
// sobra para seguir al audio, y un fallo suelto solo mueve 1 ms.
#define ARRASTRE_MS       1
// Por encima de esto se asume que ha pasado algo de verdad (un salto en
// la cancion) y se resincroniza de golpe en vez de arrastrar.
#define RESYNC_MS       250

// Margen antes de que arranque la cancion. Hay charts que ponen la primera
// nota casi en el cero, y sin esto te sueltan dentro del nivel sin tiempo de
// reaccionar. No se toca ni un tiempo de la partitura: es un silencio de
// verdad que el hilo de audio mete DELANTE de la cancion, asi que el reloj
// sale en negativo de la propia tuberia de audio y llega al cero solo (ver el
// margen de entrada en hilo_audio).
#define CUENTA_INICIO_MS  1000

// Muestra del selector: cuanto suena y cuanto se lee del .ogg como mucho.
//
// El trozo que hay que leer NO depende de estos 8 segundos sino del DEMOSTART
// de cada cancion, que en las charts reales anda entre 22 y 74 s. Con el tope
// de 4 MB entran las tres de prueba (la peor necesita ~3 MB) sin acercarse a
// comerse la RAM, que es lo que importa en una consola de 32 MB.
#define PREVIEW_MS        8000
// El ultimo segundo de la muestra se va bajando hasta cero. Se hace sobre las
// MUESTRAS, no con audsrv_set_volume: asi cae justo donde toca en el flujo de
// audio (el volumen de audsrv se aplicaria a lo que suena en ese momento, que
// va 100-200 ms por detras de lo que se acaba de mandar) y ademas no toca el
// ajuste de volumen del jugador, que es global.
#define PREVIEW_FADE_MS   1000
#define PREVIEW_MIN_BYTES (512 * 1024)
#define PREVIEW_MAX_BYTES (4 * 1024 * 1024)

// Ventanas de acierto, en milisegundos. NO son aproximadas: son las de
// OpenTaiko (CConfigIni.cs, tabla tzLevels), que a su vez son las del taiko de
// verdad. Tres zonas y no dos:
//
//   |dif| <= perfecto -> 良 (perfecto)
//   |dif| <= bueno    -> 可 (bueno)
//   |dif| <= fallo    -> 不可: se come la nota Y rompe el combo
//   mas alla          -> el golpe no engancha nada, no pasa nada
//
// La tercera zona es la que faltaba aqui: antes, un golpe a 90 ms daba
// "bueno", y en el juego de verdad eso es un fallo.
//
// Y cambian con la dificultad: hasta Normal (curso <= 1) son mas anchas.
// OpenTaiko lo decide igual, en tEasyTimeZones(): dificultad <= Normal usa la
// tabla facil. Nosotros usabamos la dura para todo, o sea que Facil se estaba
// jugando con las ventanas de Oni.
#define VENTANA_PERFECTO_MS  25
#define VENTANA_BUENO_MS     75
#define VENTANA_FALLO_MS    108

//                                   perfecto  bueno  fallo
static const int ventanas_faciles[3] = {  42,   108,   125 };  // Facil, Normal
static const int ventanas_duras[3]   = {  25,    75,   108 };  // Dificil, Oni, Edit

// La ventana del globo abre 17 ms ANTES de la cabeza. Sale de OpenTaiko
// (evaluateNodeJudge: "pChip.n発声時刻ms - 17"). Los rodillos no lo llevan.
#define ADELANTO_GLOBO_MS  17

// A que velocidad avanzan las notas hacia el juez.
#define PX_POR_MS         0.4f

// Geometria (el origen esta en el centro de la pantalla: x va de -320 a
// +320, y de -256 a +256).
#define X_JUEZ         -250.0f
#define Y_CARRIL         60.0f
#define RADIO_NOTA       26.0f
#define RADIO_JUEZ       30.0f
#define SEGMENTOS        16

// Zona segura. Un televisor de tubo se come entre el 5 y el 10 por ciento de
// cada borde, que en 640x512 son de 32 a 64 pixeles. PCSX2 enseña el
// framebuffer entero, asi que esto NO se ve en el emulador: hay que
// respetarlo a mano. Medido en TESTS_REALES/Texto.
#define SEG_X           270.0f
#define SEG_Y           215.0f

// El packet mas grande que se manda es el del menu. Medido en Texto: un
// selector de 8 lineas gasta 2221 qwords, uno de 15 llega a 3652. 8192 deja
// margen de sobra para las dos cosas y para el HUD de la partida.
#define QW_PACKET       8192

// Chart generada sola: una nota por pulso a este BPM. El .ogg de prueba
// (click140.ogg) es un metronomo a 140, asi que cada nota tiene que caer
// justo encima de un clic. Si no cuadra, el reloj esta mal.
// Autoprueba: golpea solo, en el instante justo de cada nota. Sirve para
// ejercitar el camino de juicio y, sobre todo, para ver si disparar ADPCM
// desde el hilo de audio le estropea el streaming a audsrv (mira dif_bytes
// en el registro de deriva). A 1 tiene que salir todo perfectos.
#ifndef AUTOGOLPE
#define AUTOGOLPE         0
#endif

// Con 1, el juego se recorre el ciclo entero solo: elige cancion a los 50
// frames de menu, corta la cancion a los 8 segundos y sale de resultados a
// los 50 frames. Es la unica forma de probar el ciclo en PCSX2 sin ventana,
// donde no hay mando que pulsar, y de darle las vueltas suficientes para que
// se note una fuga de hilos (una sola vuelta no la enseña).
#ifndef AUTOCICLO
#define AUTOCICLO         0
#endif
// 30 s y no 8: la primera nota de splice.tja no llega hasta pasada la
// entrada, y con un corte corto la partida terminaba sin haber visto una sola
// nota. Entonces "0 perfectos" no distingue entre "todo bien" y "la chart se
// quedo marcada como resuelta de la vuelta anterior", que es justo lo que
// esta prueba tiene que distinguir.
// Curso que fuerza el ciclo automatico. Por defecto -1 (el que toque). Se
// pone a 2 (Dificil) para las pruebas de rodillos y globos: es el curso que
// los trae mas pronto en las canciones de prueba, y en Facil casi no hay.
// Cancion que fija el ciclo automatico. -1 = va rotando. Se usa para probar
// una concreta (la de prueba de notas, por ejemplo) sin esperar a que le
// toque el turno.
#ifndef AUTOCICLO_CANCION
#define AUTOCICLO_CANCION -1
#endif

#ifndef AUTOCICLO_CURSO
#define AUTOCICLO_CURSO   -1
#endif

#ifndef AUTOCICLO_CORTE_MS
#define AUTOCICLO_CORTE_MS 30000
#endif

// Saca una linea por segundo con los fps y el peor hueco entre frames. Hace
// falta para calibrar la latencia (paso 2): la resolucion del juicio no puede
// ser mejor que el hueco entre frames.
#define LOG_FRAMES        0

// Calibracion. Ya no es un modo de compilacion: se apunta el desfase con
// signo de cada golpe SIEMPRE (cuesta un entero por nota) y la pantalla de
// resultados ofrece guardarlo. Ver informe_calibracion.
#define MAX_MUESTRAS_CAL  512
// Calibrando, la busqueda se ensancha: si vas 150 ms desviado, con la ventana
// normal (108 ms) no engancharias ni una nota y no habria nada que medir. Los
// 200 ms se quedan por debajo de medio pulso a 140 BPM (214 ms), asi que las
// ventanas de notas contiguas no se pisan.
#define VENTANA_CAPTURA_MS 200

//---------------------------------------------------------------------
// El estilo: lineas con temblor
//---------------------------------------------------------------------
// Sale de Vib-Ribbon: lineas finas, curvas facetadas a proposito y figuras
// de pocos tramos rectos. Lo que NO se hereda solo es el temblor.
//
// En PS1 las lineas vibran porque su rasterizador solo acepta coordenadas de
// vertice ENTERAS: los vertices se pegan a la rejilla de pixeles y saltan de
// uno a otro al moverse. Es un defecto del hardware, no un efecto buscado.
// El GS de PS2 tiene precision sub-pixel, asi que aqui las lineas salen
// perfectas y por eso parecerian MENOS Vib-Ribbon. Hay que ponerlo a mano, y
// puestos a ponerlo se elige su caracter: ruido CONTINUO (la linea respira)
// en vez de ruido blanco (que zumba y cansa la vista).
#define RUIDO_N          64
#define RUIDO_MASCARA    (RUIDO_N - 1)
// 180 ms entre valores: unas cinco ondulaciones por segundo.
#define RUIDO_PERIODO_MS 180.0f
// Amplitud del temblor, en pixeles. FIJA en el codigo y la misma para todo el
// mundo: es parte de como se ve el juego, no una preferencia del jugador.
//
// Llegaron a estar en el fichero de ajustes del pen, cuando habia botones
// para moverlos dentro de la cancion y tenia sentido guardar lo que afinabas.
// Al quitar esos botones, dejarlo en el pen solo conseguia que el juego se
// viera distinto segun el pen que metieras, sin forma de cambiarlo.
//
// 0,3 y 0,3 salen de afinarlo en la consola: por debajo no se nota vivo y por
// encima las figuras pequenas (un ojo del personaje mide ~10 px) se
// retuercen en vez de vibrar.
#define RUIDO_BASE_PX    0.3f
#define RUIDO_MUSICA_PX  0.3f

// Cuanto se multiplica lo que aporta la musica dentro del Gogo Time.
//
// El Gogo lo marca el autor de la chart con #GOGOSTART: es el estribillo, la
// parte gorda. Eso es MUCHO mas fiable que intentar deducir del audio donde
// esta lo importante, porque viene dicho a mano. Hasta ahora solo pintaba el
// juez de naranja; ahora ademas agita las lineas.
#define GOGO_TEMBLOR     1.8f

// Y cuanto lo multiplica el combo. Sube a escalones de 10 y llega al tope a
// los 50, asi que se nota el hito: cada diez notas seguidas las lineas se
// agitan un poco mas, y a las cinco tandas ya esta al maximo.
//
// Los dos multiplicadores se MULTIPLICAN entre si, no se suman: con 0.3 de
// base y 0.3 de musica eso deja el fondo tranquilo en ~0.45 px, el estribillo
// en ~0.6 y el estribillo con el combo a tope en ~0.8, que es justo donde
// deja de parecer un efecto y empieza a parecer un fallo.
#define COMBO_TEMBLOR    1.6f
#define COMBO_TOPE       50

// Como se dibuja una nota. El disco se lee mejor con las notas viniendo
// rapido; el anillo es mas fiel al estilo. Se elige en las opciones.
#define ESTILO_DISCO   0
#define ESTILO_ANILLO  1

// Facetada a proposito, como los aros de Vib-Ribbon.
#define LADOS_NOTA        14
#define GROSOR_ANILLO     7.0f
#define RADIO_JUEZ_INT    22.0f
#define RADIOS_JUEZ        8
// Ancho de cada diente del zigzag del alma.
//
// 26 y no 7: con el diente estrecho salia un peine de noventa puntas y se
// leia como una masa, no como una linea. Anchos hay sitio para ver cada
// tramo, que es de lo que se trata.
//
// La segunda hebra NO lleva un paso propio ni un desfase fijo: sus vertices
// van en los PUNTOS MEDIOS de los tramos de la primera, con el signo opuesto.
//
// Se probo antes con paso distinto (x1,35) y con desfase fijo, y las dos
// fallaban por lo mismo: como los anchos son aleatorios, las dos ondas
// derivan por su cuenta y cada cierto tramo acaban casi en fase, pegadas una
// encima de otra durante varios dientes. Buscar el desfase "bueno" era
// buscar suerte.
//
// Con los puntos medios no hay suerte que valga: entre dos vertices de una,
// la otra tiene uno justo en medio y con el signo contrario, asi que se
// cortan SIEMPRE, y sus vertices no pueden coincidir en x (estan a media
// distancia por construccion), que es lo que evitaba los rombos.
#define DIENTE_ALMA       26.0f

// Alto y sitio de la onda del alma: DEBAJO del carril de notas, como en la
// plantilla. El carril va de Y_CARRIL+57 a Y_CARRIL-57 (117 a 3).
#define ALMA_Y           -18.0f
#define ALMA_AMP          13.0f
// Las reglas del carril y el alma sangran hasta el borde a proposito: son
// lineas horizontales y lo que se pierda por los lados no es informacion.
// Lo que se LEE se queda dentro de SEG_X.
#define SANGRE_X         320.0f


#define BPM_CHART       140.0f
#define PULSO_INICIAL      4      // deja entrada antes de la primera nota
// Igual que TJA_MAX_NOTAS. 4096 se quedaba corto: el curso mas cargado de las
// canciones de prueba tiene 1603, pero un Oni largo de verdad pasa de 4000 y
// el desbordamiento se come las notas del FINAL, que es donde menos se nota
// hasta que llegas.
#define MAX_NOTAS       8192

// Raiz del pen. Debajo se espera la estructura de open-taiko: una carpeta
// por cancion con su .tja y su .ogg dentro. Se admiten tambien .tja sueltos
// en la raiz, y una capa mas de carpetas (las de genero que usa open-taiko),
// pero no mas: sin tope, un enlace circular en el FAT dejaria el escaneo
// dando vueltas para siempre.
#define RAIZ_PEN        "mass0:/"
#define PROF_MAX        2

// Los cinco cursos que define el formato .tja. El parser acepta el nombre o
// el numero; aqui se usan los nombres porque son los que se enseñan.
#define N_CURSOS        5
// 32 se tocaba con cualquier pen de verdad, y al llegar al tope el catalogo
// dejaba de añadir en silencio. 256 * ~470 bytes son 120 KB de los 32 MB.
#define MAX_CANCIONES   256

// .ogg que se apuntan por carpeta mientras se recorre. Con uno bastaria casi
// siempre; se guardan varios para poder emparejar por nombre con la clave
// WAVE del .tja en vez de coger el primero a ciegas.
#define MAX_OGG_CARPETA  8

// Canciones sin fichero, con la chart hecha por codigo. Las dos tiran del
// click140.ogg empotrado: sin audio no hay reloj (sale de la cola de audsrv),
// asi que "sin musica" no es una opcion. El metronomo va bien de referencia.
#define GEN_NINGUNA     0
#define GEN_METRONOMO   1
#define GEN_PRUEBA      2

// Canciones visibles a la vez en el menu. Mas no caben: 10 filas de 24 px
// son 240 de los 430 utiles de la zona segura, y quedan los de arriba para
// el titulo y los de abajo para la fila de dificultades.
#define FILAS_MENU      10

// Escupe por consola el instante de cada nota, para poder comparar lo que
// calcula la PS2 contra la referencia en Python. Ver README.
#define VOLCAR_CHART      0

// Mapeo del tambor de verdad (TaTaCon), que es el que se copia aqui:
//
//   ROJO (don), el parche central     izquierda: IZQUIERDA y ABAJO de la cruceta
//                                     derecha:   CIRCULO y CRUZ
//   AZUL (ka), los bordes             izquierda: L1
//                                     derecha:   R1
//
// Dos botones por color no es un capricho: es lo que permite alternar manos.
// Con uno solo, los cursos rapidos son fisicamente imposibles —en Oni hay
// huecos de 100 ms y en Edit de 50— porque no da tiempo a soltar y volver a
// pulsar el mismo boton.
//
// Se acumulan en una mascara y se mira el flanco de CUALQUIERA de ellos, no
// de uno concreto: golpear R1 con L1 todavia hundido tiene que contar.
// Jugando da igual con que parche rojo golpees, pero en las opciones no: para
// mover una barra de volumen hacen falta dos sentidos y el tambor no tiene mas
// botones. Ahi el parche IZQUIERDO baja y el DERECHO sube, que es lo que sale
// solo si te imaginas la barra delante.
#define BOTONES_DON_IZQ  (PAD_LEFT | PAD_DOWN)
#define BOTONES_DON_DER  (PAD_CIRCLE | PAD_CROSS)
#define BOTONES_DON  (BOTONES_DON_IZQ | BOTONES_DON_DER)
#define BOTONES_KA   (PAD_L1 | PAD_R1)

#define NOTA_DON      0
#define NOTA_KA       1
#define NOTA_RODILLO  2
#define NOTA_GLOBO    3

// Cuanto mas grande se dibuja una nota grande.
#define RADIO_GRANDE  (RADIO_NOTA * 1.45f)

#define JUICIO_NADA      0
#define JUICIO_PERFECTO  1
#define JUICIO_BUENO     2
#define JUICIO_FALLO     3

// Sonidos que el bucle de dibujo puede pedirle al hilo de audio
#define SFX_DON     0
#define SFX_BIGDON  1
#define SFX_CANCEL  2
#define N_SFX       3

// cpu_ticks() lee el registro Count del COP0, que en el R5900 cuenta a la
// frecuencia del NUCLEO (294.912 MHz), no a kBUSCLK (147.456 MHz, que es
// el reloj del bus y el que usan los Timer0-3 del EE). Medido: la cuenta
// avanza exactamente al doble que kBUSCLK, ratio 2.0000.
#define TICKS_POR_SEG  294912000LL

// En una PS2 real no hay consola: printf va al EE console, que solo se ve
// con PCSX2 o con un ps2client por red. Asi que mientras se carga, los
// mensajes salen TAMBIEN por pantalla con scr_printf. En cuanto el GS toma
// el control (init_gs) hay que apagarlo, o el registro de deriva del hilo
// de audio escribiria texto encima del juego.
static int pantalla_texto = 0;

#define LOG(...) do { \
	if (pantalla_texto) scr_printf(__VA_ARGS__); \
	printf(__VA_ARGS__); \
} while (0)

//---------------------------------------------------------------------
// Datos empotrados en el .elf (bin2c en el Makefile)
//---------------------------------------------------------------------
extern unsigned char iomanX_irx[];      extern unsigned int size_iomanX_irx;
extern unsigned char fileXio_irx[];     extern unsigned int size_fileXio_irx;
extern unsigned char bdm_irx[];         extern unsigned int size_bdm_irx;
extern unsigned char bdmfs_fatfs_irx[]; extern unsigned int size_bdmfs_fatfs_irx;
extern unsigned char usbd_irx[];        extern unsigned int size_usbd_irx;
extern unsigned char usbmass_bd_irx[];  extern unsigned int size_usbmass_bd_irx;
extern unsigned char audsrv_irx[];      extern unsigned int size_audsrv_irx;

extern unsigned char don_adp[];         extern unsigned int size_don_adp;
extern unsigned char bigdon_adp[];      extern unsigned int size_bigdon_adp;
extern unsigned char cancel_adp[];      extern unsigned int size_cancel_adp;

// Cancion de reserva si no hay pen conectado
extern unsigned char click140_ogg[];    extern unsigned int size_click140_ogg;

extern void *_gp;


//---------------------------------------------------------------------
// Chart
//---------------------------------------------------------------------
typedef struct {
	int  tiempo_ms;
	int  fin_ms;    // rodillos y globos: final del tramo. Si no, == tiempo_ms
	int  tipo;      // NOTA_DON / NOTA_KA / NOTA_RODILLO / NOTA_GLOBO
	int  grande;    // 1 si venia como 3, 4 o 6
	int  golpes;    // globos: los que hacen falta
	int  dados;     // rodillos: golpes dados. globos: los que faltan
	int  resuelta;  // ya se golpeo o ya se paso de largo
	float scroll;   // #SCROLL: multiplica la distancia a la que se dibuja
	int  gogo;      // 1 si cae dentro de un tramo de Gogo Time
} nota_t;

static nota_t notas[MAX_NOTAS];
static int    n_notas = 0;

// Con cuanta antelacion hay que empezar a mirar notas para dibujarlas. Sale
// del #SCROLL mas bajo de la chart: cuanto menor el scroll, mas lejos en el
// tiempo esta una nota que ya se ve en pantalla. Antes esto era implicito
// —se cortaba en cuanto una nota salia por la derecha— y con scroll variable
// esa cuenta ya no vale: una nota posterior con scroll bajo puede estar en
// pantalla cuando la anterior todavia no ha entrado.
static int ventana_dibujo_ms = 2200;

// Ventana dentro de la cual un golpe puede enganchar una nota. En una partida
// normal es la de fallo; con el metronomo se ensancha a VENTANA_CAPTURA_MS,
// porque si vas 150 ms desviado con la ventana normal no engancharias ni una
// nota y no habria nada que medir. Variable y no #define porque calibrar ya
// no es recompilar: es elegir el metronomo en el menu.
static int ventana_activa   = VENTANA_FALLO_MS;
static int ventana_perfecto = VENTANA_PERFECTO_MS;
static int ventana_bueno    = VENTANA_BUENO_MS;
static int modo_calibracion = 0;

// El desfase que se aplica al reloj. Arranca con el de compilacion y lo pisa
// el fichero de ajustes del pen si lo hay.
static int offset_ms = OFFSET_LATENCIA_MS;

// Volumenes, en rayitas de 0 a VOL_PASOS. Se guardan asi y no en porcentaje
// porque asi es como se enseñan y como se mueven: lo que se guarda es lo que
// el jugador ve, sin conversiones que redondeen raro al releer.
#define VOL_PASOS  10
static int vol_musica = VOL_PASOS;
static int vol_sonido = VOL_PASOS;

// Estilo de dibujo. Vive en el fichero de ajustes del pen como todo lo demas.
static int estilo_nota  = ESTILO_DISCO;
// Amplitud que sale de verdad ahora mismo: base + musica * energia. La
// calcula el bucle de dibujo en cada fotograma y la usan las primitivas.
static float amp_temblor  = RUIDO_BASE_PX;
static float ruido_tabla[RUIDO_N];

// Canales de voz del SPU2 que maneja audsrv. Los golpes salen con ch = -1 (el
// primero que este libre), asi que no hay un canal fijo al que ponerle el
// volumen: hay que ponerselo a todos.
#define N_CANALES_SPU  24

//---------------------------------------------------------------------
// Catalogo de canciones
//
// Se llena recorriendo el pen (escanear_pen). Ni el menu ni el bucle de
// juego saben de donde ha salido: para ellos es un array y ya.
//---------------------------------------------------------------------
static const char *nombre_curso[N_CURSOS] = {
	"Facil", "Normal", "Dificil", "Oni", "Edit"
};

//---------------------------------------------------------------------
// El alma (魂ゲージ)
//---------------------------------------------------------------------
// De OpenTaiko: CAct演奏ゲージ共通.cs (el llenado) y HGaugeMethods.cs (la
// norma). Empieza en cero y solo se mira AL FINAL: en taiko no se muere a
// mitad de cancion, se aprueba o no se aprueba.
//
//   tasa  = 70,7 si LEVEL <= 7 | 70,0 si LEVEL = 8 | 75,0 si LEVEL >= 9
//   daño  = 0,625 si LEVEL <= 8 | 2,0 si LEVEL >= 9
//   subida_por_nota = 10000 / (notas * tasa)     <- en puntos de %
//
//   perfecto -> +subida      bueno -> +subida/2      fallo -> -subida*daño
//
// La "tasa" es literalmente a que porcentaje de las notas se llena el alma
// entera: con 70,7, clavando el 70,7 % de las notas ya estas a 100.
//
// Rodillos y globos NO la tocan, igual que en la puntuacion: solo cuentan las
// notas que se pueden fallar.
static const int norma_curso[N_CURSOS] = { 60, 70, 70, 80, 80 };

// El LEVEL manda sobre la dificultad a partir de 11: un Oni del 11 pide 88 y
// no 80.
static int norma_del_alma(int curso, int nivel)
{
	if (nivel >= 13) return 96;
	if (nivel == 12) return 92;
	if (nivel == 11) return 88;
	return norma_curso[curso];
}
// Lo que entiende el parser (que acepta nombre o numero). Se mandan los
// numeros para no depender de como venga escrito el #COURSE del fichero.
static const char *clave_curso[N_CURSOS] = { "0", "1", "2", "3", "4" };

typedef struct {
	char titulo[64];       // el #TITLE, o el nombre del fichero si no hay
	char ruta_tja[192];    // ruta completa, ya con carpetas
	char ruta_ogg[192];    // ruta completa del audio, resuelta al escanear
	int  nivel[N_CURSOS];  // el #LEVEL de cada curso, -1 si no existe
	int  generada;         // GEN_*: chart hecha por codigo, sin .tja ni .ogg
	// Para la muestra del selector: donde empieza (DEMOSTART, en segundos) y
	// cuanto dura la chart mas larga. Lo segundo no es un dato que se enseñe:
	// sirve para estimar cuantos BYTES del .ogg hay que leer para llegar al
	// DEMOSTART sin cargar el fichero entero (ver cargar_preview_usb).
	float demostart;
	int   dur_ms;
} cancion_t;

static cancion_t canciones[MAX_CANCIONES];
static int       n_canciones = 0;

// Las del pen ocupan las n_visibles primeras y son las unicas que salen en la
// lista; detras van las generadas (metronomo y nivel de prueba), que se juegan
// desde el menu de opciones. Siguen en el mismo array porque todo lo demas
// —cargar, parsear, jugar— las trata igual que a cualquier otra.
static int       n_visibles = 0;

// Cache de un hueco para el .ogg. Volver a leer 4 MB del pen cuesta varios
// segundos en la consola (USB 1.1), asi que si se repite cancion no se
// vuelve a leer. Solo una: no hay motivo para guardar dos y si lo hubiera,
// la RAM son 32 MB.
// La ultima medida de cargar_del_usb, en texto y ya formateada. En una PS2
// real no hay consola: printf no va a ninguna parte que se pueda leer, y la
// carga del .ogg ocurre cuando el GS ya manda, asi que sin esto el dato que
// mas falta hace (cuanto tarda de verdad el pen) no se ve nunca.
static char           ultima_carga[80] = "";

static char           ogg_en_cache[192] = "";
static unsigned char *ogg_buffer = NULL;
static long           ogg_cache_tam = 0;

//---------------------------------------------------------------------
// Estado compartido entre el bucle de dibujo y el hilo de audio.
//
// Regla: el hilo de audio escribe el reloj y lee las peticiones de
// sonido; el bucle de dibujo lee el reloj y escribe las peticiones.
// Nadie mas toca audsrv.
//---------------------------------------------------------------------

// Reloj. Se publica con un contador de version (seqlock) porque son dos
// palabras y el cambio de hilo puede caer justo en medio: si el lector
// ve la version impar o distinta antes y despues, reintenta.
static volatile unsigned int reloj_version   = 0;
static volatile long long    reloj_ms_base   = 0;
static volatile unsigned int reloj_ticks_base = 0;

static volatile int   audio_listo     = 0;  // ya se sabe formato y duracion
static volatile int   audio_terminado = 0;
// El bucle de juego la pone para cortar la cancion antes de tiempo. Sin
// esto, main llamaba a audsrv_quit() mientras el hilo de audio seguia
// decodificando y usando audsrv: una carrera de las que acaban petando.
static volatile int   parar_audio     = 0;
// Para que detener_audio() se pueda llamar dos veces sin repetir la espera.
static int            audio_parado    = 0;
// Pausa. La ejecuta el hilo de audio, que es el unico que puede tocar audsrv
// mientras suena la cancion; el bucle de dibujo solo la pide y espera el
// acuse. Mientras pausa_activa no sea 1, la musica sigue sonando.
static volatile int   pausa_pedida  = 0;
static volatile int   pausa_activa  = 0;
static int            hilo_audio_id = -1;

// Con la pausa puesta el reloj NO puede seguir corriendo. leer_reloj_bruto_ms
// interpola con cpu_ticks() desde la ultima marca publicada, asi que sin esto
// una pausa de treinta segundos volveria con el reloj treinta segundos por
// delante, y un solo fotograma con ese valor manda al saco de fallos todas
// las notas que quedaban. Se congela DENTRO del lector, no en el bucle de
// juego: asi protege a cualquiera que lea el reloj y no solo al sitio que uno
// se acuerde de tapar.
static volatile int       reloj_congelado = 0;
static volatile long long reloj_ms_pausa  = 0;

// Energia de la musica, 0..1000. La saca el hilo de audio del PCM que acaba
// de decodificar (esta ahi mismo, no cuesta nada) y la lee el dibujo para
// mover el temblor. Una sola palabra de 32 bits: en el R5900 esa carga no se
// parte, asi que no hace falta seqlock como en el reloj.
static volatile int   energia         = 0;

static volatile int   audio_freq      = 44100;
static volatile int   audio_canales   = 2;
static volatile int   duracion_ms     = 0;

// Peticiones de sonido: el dibujo incrementa, el audio iguala.
static volatile int sfx_pedido[N_SFX];
static          int sfx_servido[N_SFX];

// Fuente del .ogg (ya cargado entero en RAM)
static const unsigned char *ogg_datos = NULL;
static long                 ogg_tam   = 0;

static audsrv_adpcm_t muestra[N_SFX];

//---------------------------------------------------------------------
// Fuente en memoria para libvorbisfile
//---------------------------------------------------------------------
typedef struct {
	const unsigned char *datos;
	long tam;
	long pos;
} fuente_t;

static size_t mem_read(void *ptr, size_t size, size_t nmemb, void *ds)
{
	fuente_t *f = (fuente_t *)ds;
	long total;

	if (f == NULL || size == 0) return 0;

	total = (long)(size * nmemb);
	if (total > f->tam - f->pos) total = f->tam - f->pos;
	if (total < 0) total = 0;
	if (total > 0) memcpy(ptr, f->datos + f->pos, (size_t)total);
	f->pos += total;
	return (size_t)(total / (long)size);
}

static int mem_seek(void *ds, ogg_int64_t offset, int whence)
{
	fuente_t *f = (fuente_t *)ds;
	long nuevo;

	if (f == NULL) return -1;
	if (whence == SEEK_SET)      nuevo = (long)offset;
	else if (whence == SEEK_CUR) nuevo = f->pos + (long)offset;
	else if (whence == SEEK_END) nuevo = f->tam + (long)offset;
	else return -1;

	if (nuevo < 0 || nuevo > f->tam) return -1;
	f->pos = nuevo;
	return 0;
}

static int  mem_close(void *ds) { (void)ds; return 0; }
static long mem_tell(void *ds)  { return ((fuente_t *)ds)->pos; }

static ov_callbacks cbs_mem = { mem_read, mem_seek, mem_close, mem_tell };

//---------------------------------------------------------------------
// Reloj de cancion
//---------------------------------------------------------------------

// Peor error de interpolacion visto en el ultimo segundo, en ms. Se mide
// comparando lo que el interpolador iba a contestar justo antes de
// refrescar el reloj con el valor bueno recien calculado.
static int error_interp_max = 0;
static int reloj_arrancado = 0;
// Mientras la cola de audsrv se esta llenando, audsrv_wait_audio no bloquea
// y se mandan muchos trozos seguidos: la medida cruda se queda pegada a cero
// mientras el tiempo real corre, y llega a desviarse >130 ms. Arrastrar eso a
// 1 ms por trozo tardaria segundo y medio en cuadrar, y durante ese rato las
// notas del principio de la cancion se juzgarian contra un reloj mal. Asi que
// hasta que la cola no se llena, el reloj se engancha en duro.
static int reloj_estable = 0;

static long long leer_reloj_bruto_ms(void);
static void detener_audio(void);

// Solo lo llama el hilo de audio. ms_bruto es la medida cruda sacada de
// audsrv_queued(); lo que se publica es el reloj arrastrado hacia ella.
static void publicar_reloj(long long ms_bruto)
{
	long long ms = ms_bruto;

	if (reloj_arrancado) {
		long long actual = leer_reloj_bruto_ms();
		long long err = ms_bruto - actual;
		int abs_err = (int)(err < 0 ? -err : err);

		if (abs_err > error_interp_max) error_interp_max = abs_err;

		if (!reloj_estable || abs_err > RESYNC_MS) {
			ms = ms_bruto;               // salto real: resincroniza
		} else {
			if (err >  ARRASTRE_MS) err =  ARRASTRE_MS;
			if (err < -ARRASTRE_MS) err = -ARRASTRE_MS;
			ms = actual + err;
		}
	}

	reloj_version++;              // impar: escribiendo
	__asm__ __volatile__("sync.l" ::: "memory");
	reloj_ms_base    = ms;
	reloj_ticks_base = cpu_ticks();
	__asm__ __volatile__("sync.l" ::: "memory");
	reloj_version++;              // par: ya se puede leer
	reloj_arrancado = 1;
}

// Lo llama el bucle de dibujo. Devuelve los milisegundos de cancion que
// suenan AHORA, interpolando con el contador de ciclos entre trozo y
// trozo. La resta de ticks se hace en 32 bits a proposito: cpu_ticks da
// vueltas cada ~29 s, y una resta sin signo de 32 bits sale bien igual
// mientras el intervalo sea corto (aqui es de un frame).
static long long leer_reloj_bruto_ms(void)
{
	unsigned int v1, v2;
	long long ms;
	unsigned int base, delta;

	// En pausa el tiempo no corre: se devuelve el instante en el que se
	// paro, y no lo que diga el contador de ciclos.
	if (reloj_congelado) return reloj_ms_pausa;

	do {
		v1 = reloj_version;
		ms   = reloj_ms_base;
		base = reloj_ticks_base;
		__asm__ __volatile__("sync.l" ::: "memory");
		v2 = reloj_version;
	} while (v1 != v2 || (v1 & 1));

	delta = (unsigned int)(cpu_ticks() - base);
	ms += (long long)delta * 1000 / TICKS_POR_SEG;

	return ms;
}

// Lo que usa el bucle de juego: igual pero con la latencia ya aplicada.
static int leer_reloj_ms(void)
{
	return (int)(leer_reloj_bruto_ms() + offset_ms);
}

// Para el reloj en el punto que esta sonando ahora mismo.
static void congelar_reloj(void)
{
	reloj_ms_pausa  = leer_reloj_bruto_ms();
	reloj_congelado = 1;
}

// Y lo vuelve a arrancar en "ms". reloj_arrancado a 0 hace que publicar_reloj
// se crea ese valor tal cual en vez de arrastrarlo hacia el anterior, que
// lleva parado todo lo que haya durado la pausa. El orden importa: primero
// publicar y descongelar despues, para que nadie llegue a leer la marca
// vieja con el reloj ya corriendo.
static void descongelar_reloj(long long ms)
{
	reloj_arrancado = 0;
	publicar_reloj(ms);
	reloj_congelado = 0;
}

//---------------------------------------------------------------------
// Hilo de audio: decodifica, alimenta a audsrv, publica el reloj y
// dispara los sonidos de golpe que le pida el bucle de dibujo.
//---------------------------------------------------------------------
static int hilo_audio(void *arg)
{
	static fuente_t      fuente;
	static OggVorbis_File vf;
	// Alineado a 16: el calculo de energia lo recorre como muestras de 16
	// bits, y de paso es lo que quiere el DMA que se lo lleva al IOP.
	static char          pcm[TROZO_PCM] __attribute__((aligned(16)));
	vorbis_info *info;
	audsrv_fmt_t fmt;
	int ret, bitstream, i;
	long leido = 0;   // puede salirse del bucle antes de asignarla (parar_audio)
	long long enviados = 0;
	// Bytes de silencio que van delante de la cancion. Es lo que hay que
	// restar en todas las cuentas de posicion: enviados incluye el silencio y
	// la cancion empieza cuando se acaba.
	long long silencio_bytes = 0;
	int bps;
	int en_cola_prev = 0;
	int en_cola_max = 0;
	int colas_malas = 0;
	unsigned int ticks_prev;
	long long ticks_totales = 0;
	long long siguiente_log_ms = 1000;

	(void)arg;

	fuente.datos = ogg_datos;
	fuente.tam   = ogg_tam;
	fuente.pos   = 0;

	memset(&vf, 0, sizeof(vf));
	ret = ov_open_callbacks(&fuente, &vf, NULL, 0, cbs_mem);
	if (ret != 0) {
		LOG("ov_open_callbacks fallo: %d\n", ret);
		if (ret == -132)
			LOG("(-132: falta el framing.o de libogg_fix, ver Audio_Ogg/README.md)\n");
		audio_terminado = 1;
		audio_listo = 1;
		// ExitDeleteThread y no ExitThread: el hilo se crea de nuevo en cada
	// cancion, y ExitThread deja el TCB ocupado. Con unas cuantas partidas
	// seguidas se acabarian los huecos de hilo y CreateThread empezaria a
	// fallar. Que se borre a si mismo es ademas lo unico seguro: main no
	// puede hacer DeleteThread mirando audio_terminado, porque esa bandera
	// se pone ANTES de salir y borraria un hilo todavia en ejecucion.
	ExitDeleteThread();
		return 0;
	}

	info = ov_info(&vf, -1);
	audio_freq    = info->rate;
	audio_canales = info->channels;
	bps = audio_freq * audio_canales * 2;
	duracion_ms = (int)(ov_time_total(&vf, -1) * 1000.0);

	LOG("Audio: %d canales, %d Hz, %d ms\n",
	    audio_canales, audio_freq, duracion_ms);

	fmt.freq     = audio_freq;
	fmt.bits     = 16;
	fmt.channels = audio_canales;
	if (audsrv_set_format(&fmt) != 0) {
		LOG("audsrv_set_format fallo: %s\n", audsrv_get_error_string());
		ov_clear(&vf);
		audio_terminado = 1;
		audio_listo = 1;
		// ExitDeleteThread y no ExitThread: el hilo se crea de nuevo en cada
	// cancion, y ExitThread deja el TCB ocupado. Con unas cuantas partidas
	// seguidas se acabarian los huecos de hilo y CreateThread empezaria a
	// fallar. Que se borre a si mismo es ademas lo unico seguro: main no
	// puede hacer DeleteThread mirando audio_terminado, porque esa bandera
	// se pone ANTES de salir y borraria un hilo todavia en ejecucion.
	ExitDeleteThread();
		return 0;
	}
	audsrv_set_volume(MAX_VOLUME * vol_musica / VOL_PASOS);

	//--- Margen de entrada ---
	//
	// El segundo que se da antes de la primera nota es un segundo de SILENCIO
	// DE VERDAD metido delante de la cancion, no una cuenta atras fingida con
	// el reloj parado. La diferencia importa:
	//
	// Con el reloj congelado y movido a mano por render, al soltarlo habia que
	// traspasarselo al hilo de audio justo cuando la cola de audsrv todavia se
	// estaba llenando, que es el rato en el que la medida cruda se queda
	// pegada y publicar_reloj engancha en duro (ver reloj_estable). El reloj
	// pegaba un escalon delante de las narices del jugador, y despues de un
	// segundo liso se veia clarisimo: era el tiron del arranque.
	//
	// Mandando silencio, el reloj sale de la tuberia de audio desde el primer
	// momento, la cola se llena DURANTE el silencio y cuando cruza el cero ya
	// esta estable. No hay traspaso, asi que no hay escalon.
	//
	// En bytes y no en ms: el silencio se manda en trozos de TROZO_PCM y no
	// cae justo en el milisegundo, asi que redondearlo dejaria un desfase fijo
	// de hasta 10 ms en TODAS las notas. Restando los mismos bytes que se
	// mandan, el cero de la cancion queda exacto.
	silencio_bytes = ((long long)bps * CUENTA_INICIO_MS / 1000 / TROZO_PCM)
	                 * TROZO_PCM;
	memset(pcm, 0, sizeof(pcm));

	// El reloj publicado ANTES de audio_listo, como siempre: main deja correr
	// a render en cuanto ve esa bandera, y render lee el reloj en su primer
	// fotograma. Sin esto leeria una marca a cero con ticks a cero, o sea el
	// contador de ciclos crudo: un numero enorme y sin sentido.
	publicar_reloj(-(silencio_bytes * 1000 / bps));
	ticks_prev = cpu_ticks();
	audio_listo = 1;

	LOG("--- registro de deriva (una linea por segundo) ---\n");
	LOG("  t_bytes  t_reloj t_ciclos  dif_bytes  dif_ciclos  err_interp colas_malas\n");

	for (;;) {
		long long ms;
		int en_cola;

		// ov_read(..., 0, 2, 1, ...) = little endian, 16 bits, con signo
		if (parar_audio) {
			LOG("Audio cortado a peticion del juego\n");
			break;
		}

		// --- Pausa ---
		//
		// Vaciar la cola del IOP es obligatorio, no una limpieza. Si solo se
		// dejara de mandar PCM, al vaciarse sola audsrv no se calla: se queda
		// repitiendo el ultimo trozo, que es la nota clavada que ya se vio al
		// acabar una cancion.
		//
		// Y como se vacia, la cancion sigue exactamente por el byte que toca
		// decodificar: enviados/bps ES la posicion. Por eso no hace falta
		// buscar dentro del .ogg al reanudar. Lo unico que se pierde son los
		// ~100 ms que quedaban en la cola.
		if (pausa_pedida) {
			audsrv_stop_audio();
			congelar_reloj();
			pausa_activa = 1;

			// SleepThread y no una espera en vacio: el bucle de dibujo gira
			// (graph_wait_vsync sondea, no duerme), asi que este hilo, que
			// tiene mas prioridad, dejaria al juego sin CPU y la pausa se
			// veria igual que un cuelgue. WakeupThread lleva cuenta, asi que
			// si llegara antes de dormirse no se perderia.
			SleepThread();

			// Lo primero al despertar: puede que el que despierta sea
			// detener_audio para volver al menu, no la reanudacion.
			if (parar_audio) {
				pausa_activa = 0;
				LOG("Audio cortado desde la pausa\n");
				break;
			}

			// Se vuelve a fijar formato y volumen, igual que al arrancar la
			// cancion. No deberia hacer falta, porque el formato no ha
			// cambiado; se hace porque NO esta comprobado que audsrv acepte
			// PCM otra vez despues de un stop, y si no lo aceptara la cancion
			// volveria muda de la pausa sin decir nada.
			if (audsrv_set_format(&fmt) != 0)
				LOG("audsrv_set_format al reanudar: %s\n",
				    audsrv_get_error_string());
			audsrv_set_volume(MAX_VOLUME * vol_musica / VOL_PASOS);

			// La cola arranca vacia otra vez, asi que la medida cruda vuelve
			// a estar pegada mientras se llena: mismo trato que al empezar la
			// cancion.
			en_cola_prev  = 0;
			en_cola_max   = 0;
			reloj_estable = 0;
			descongelar_reloj((enviados - silencio_bytes) * 1000 / bps);

			// Y esto lo ULTIMO, ya con el reloj republicado: es la señal de
			// que el bucle de dibujo puede volver a leerlo.
			pausa_activa = 0;
		}

		// Mientras dure el margen de entrada, la fuente es el trozo a cero de
		// arriba en vez del .ogg. Todo lo demas del bucle (cola, reloj,
		// sonidos de golpe) es identico: para el resto del hilo el silencio
		// es cancion como cualquier otra.
		if (enviados < silencio_bytes) {
			leido = TROZO_PCM;
		} else {
			leido = ov_read(&vf, pcm, TROZO_PCM, 0, 2, 1, &bitstream);
			if (leido <= 0) break;
		}

		audsrv_wait_audio((int)leido);
		audsrv_play_audio(pcm, (int)leido);
		enviados += leido;

		// Energia del trozo, para el temblor de las lineas. Se mira una
		// muestra de cada ocho: para mover un temblor da de sobra y deja el
		// coste en nada.
		{
			short *m = (short *)pcm;
			int n = (int)leido / 2, k, cuenta = 0;
			long suma = 0;

			for (k = 0; k < n; k += 8) {
				int v = m[k];
				suma += (v < 0) ? -v : v;
				cuenta++;
			}
			if (cuenta > 0) {
				// El divisor decide cuanto RECORRIDO tiene la medida, y ahi
				// estaba el problema: con 6000, un tema masterizado moderno
				// (que va comprimido y por tanto plano de volumen) se pasa la
				// cancion entera pegado al tope. Saturado no varia, y si no
				// varia el temblor tampoco.
				//
				// Con 12000 casi nunca llega al tope, asi que usa el rango de
				// verdad y la diferencia entre lo flojo y lo fuerte se nota.
				int e = (int)(suma / cuenta) * 1000 / 12000;
				if (e > 1000) e = 1000;
				energia = e;
			}
		}

		// Reloj: lo que hemos mandado menos lo que aun no ha sonado.
		//
		// audsrv_queued() devuelve 0 de vez en cuando (mas o menos una
		// vez por segundo) justo cuando su buffer circular del IOP da la
		// vuelta. No es un corte de audio: la llamada siguiente ya dice
		// ~17000 otra vez y los bytes no dejan de fluir. Si se colara,
		// el reloj pegaria un salto de +100 ms. Se descarta y se reusa
		// la ultima lectura buena.
		en_cola = audsrv_queued();
		if (en_cola <= 0) {
			colas_malas++;
			// Si la mala es la primera de todas no hay lectura anterior
			// que reusar, y creerse un cero daria una posicion enorme.
			// Se salta el trozo y ya se refrescara en el siguiente.
			if (en_cola_prev <= 0) continue;
			en_cola = en_cola_prev;
		} else {
			en_cola_prev = en_cola;
		}

		// La cola crece sin parar hasta que se llena; el primer trozo en
		// el que deja de crecer marca el regimen estable.
		if (!reloj_estable) {
			if (en_cola < en_cola_max) reloj_estable = 1;
			else                       en_cola_max = en_cola;
		}

		ms = (enviados - en_cola - silencio_bytes) * 1000 / bps;
		publicar_reloj(ms);

		// cpu_ticks es de 32 bits y da la vuelta cada ~29 s. Aqui se
		// acumula en 64: este bucle pasa cada ~10 ms, asi que nunca se
		// pierde una vuelta.
		{
			unsigned int ahora_t = cpu_ticks();
			ticks_totales += (unsigned int)(ahora_t - ticks_prev);
			ticks_prev = ahora_t;
		}

		// Sonidos de golpe que haya pedido el bucle de dibujo.
		for (i = 0; i < N_SFX; i++) {
			if (sfx_servido[i] != sfx_pedido[i]) {
				sfx_servido[i] = sfx_pedido[i];
				// Se mira el retorno porque detener_audio() llama ahora a
				// audsrv_stop_audio() entre canciones. Si eso tocara el
				// estado del ADPCM, los golpes se quedarian mudos de la
				// segunda cancion en adelante y NADA lo diria: sfx_pedido
				// sube igual y la puntuacion sale identica.
				if (audsrv_ch_play_adpcm(i == SFX_CANCEL ? 0 : -1,
				                         &muestra[i]) < 0)
					LOG("adpcm %d fallo: %s\n", i,
					    audsrv_get_error_string());
			}
		}

		// Comprobacion de deriva. Lo que importa NO es que el reloj
		// arranque en cero (lleva un desfase constante), sino que
		// avance al mismo ritmo que el contador de ciclos del EE: la
		// columna "dif" tiene que quedarse acotada, no crecer.
		if (ms >= siguiente_log_ms) {
			long long t_bytes  = (enviados - silencio_bytes) * 1000 / bps;
			long long t_ciclos = ticks_totales * 1000 / TICKS_POR_SEG;
			LOG("  %7d %8d %8d %10d %11d %11d %6d\n",
			    (int)t_bytes, (int)leer_reloj_bruto_ms(), (int)t_ciclos,
			    (int)(t_bytes - ms), (int)(ms - t_ciclos),
			    error_interp_max, colas_malas);
			error_interp_max = 0;
			siguiente_log_ms += 1000;
		}
	}

	LOG("Bucle de decodificacion terminado (ov_read=%d)\n", (int)leido);
	if (leido < 0) LOG("ov_read devolvio %d\n", (int)leido);

	// Aqui NO se espera a que se vacie la cola de audsrv, y es a proposito.
	//
	// El hilo de dibujo gira en vacio: graph_wait_vsync y draw_wait_finish
	// de ps2sdk son bucles de sondeo sobre registros del GS, no duermen
	// (comprobado desensamblando: "beqz v0, <atras>"). O sea que main
	// nunca cede la CPU por su cuenta.
	//
	// Con eso, cualquier espera activa en este hilo bloquea el programa,
	// se ponga la prioridad que se ponga:
	//   - por encima de main -> este hilo gira y deja al juego sin CPU;
	//   - por debajo de main -> main gira y deja a este hilo sin CPU, y
	//     entonces audio_terminado no llega a ponerse nunca y la partida
	//     no acaba.
	// Las dos versiones se colgaron de verdad antes de entender esto.
	//
	// Y no hace falta esperar: lo que queda en la cola lo sigue tocando el
	// IOP el solo, y el reloj sigue avanzando con cpu_ticks desde la
	// ultima marca publicada.

	LOG("Fin del audio: %d bytes PCM enviados (%d lecturas de cola malas)\n",
	    (int)enviados, colas_malas);
	ov_clear(&vf);
	audio_terminado = 1;
	// ExitDeleteThread y no ExitThread: el hilo se crea de nuevo en cada
	// cancion, y ExitThread deja el TCB ocupado. Con unas cuantas partidas
	// seguidas se acabarian los huecos de hilo y CreateThread empezaria a
	// fallar. Que se borre a si mismo es ademas lo unico seguro: main no
	// puede hacer DeleteThread mirando audio_terminado, porque esa bandera
	// se pone ANTES de salir y borraria un hilo todavia en ejecucion.
	ExitDeleteThread();
	return 0;
}

//---------------------------------------------------------------------
// Chart: una nota por pulso, alternando don y ka.
//---------------------------------------------------------------------
// Vuelca la chart parseada al array del motor. Se copia en vez de usar la
// del parser porque el motor le anade el estado "resuelta" por nota.
//
// De momento las notas grandes (3 y 4 del .tja) se tratan como normales:
// el parser ya las distingue, pero el juego todavia no. En el curso Easy
// de splice.tja son 4 de 189.
static void chart_desde_tja(const tja_chart_t *ch)
{
	int i;

	n_notas = 0;
	for (i = 0; i < ch->n_notas && n_notas < MAX_NOTAS; i++) {
		if (ch->notas[i].tiempo_ms < 0) continue;   // antes de que empiece el audio

		switch (ch->notas[i].tipo) {
		case TJA_DON:     notas[n_notas].tipo = NOTA_DON;     break;
		case TJA_KA:      notas[n_notas].tipo = NOTA_KA;      break;
		case TJA_RODILLO: notas[n_notas].tipo = NOTA_RODILLO; break;
		default:          notas[n_notas].tipo = NOTA_GLOBO;   break;
		}
		notas[n_notas].tiempo_ms = ch->notas[i].tiempo_ms;
		notas[n_notas].fin_ms    = ch->notas[i].fin_ms;
		notas[n_notas].grande    = ch->notas[i].grande;
		notas[n_notas].golpes    = ch->notas[i].golpes;
		notas[n_notas].dados     = 0;
		notas[n_notas].resuelta  = 0;
		notas[n_notas].scroll    = ch->notas[i].scroll;
		notas[n_notas].gogo      = ch->notas[i].gogo;

		// Un scroll de cero dejaria la nota clavada en el juez desde el
		// principio de la cancion. No deberia llegar (el parser cambia los
		// valores complejos por 1), pero cuesta una linea.
		if (notas[n_notas].scroll == 0.0f) notas[n_notas].scroll = 1.0f;

		// Red de seguridad: un tramo con el final antes del principio seria
		// una ventana de golpe imposible y una barra dibujada del reves.
		// El parser ya lo evita, pero esto cuesta una linea.
		if (notas[n_notas].fin_ms < notas[n_notas].tiempo_ms)
			notas[n_notas].fin_ms = notas[n_notas].tiempo_ms;

		n_notas++;
	}

	// La ventana de dibujo, a partir del scroll mas bajo: una nota se ve
	// cuando su distancia baja de ~670 px, o sea a los
	// 670 / (PX_POR_MS * scroll) ms de llegar.
	{
		float sm = (ch->scroll_min > 0.05f) ? ch->scroll_min : 0.2f;

		ventana_dibujo_ms = (int)(670.0f / (PX_POR_MS * sm)) + 500;
		if (ventana_dibujo_ms > 30000) ventana_dibujo_ms = 30000;
	}

	LOG("Chart: %s [%s %s] %d entradas, ultima en %d ms\n",
	    ch->titulo, ch->curso, ch->nivel, n_notas, ch->dur_ms);
	LOG("  scroll min %.2f -> ventana de dibujo %d ms\n",
	    ch->scroll_min, ventana_dibujo_ms);
	if (ch->avisos_scroll)
		LOG("  AVISO: %d #SCROLL complejos, esas notas van a velocidad normal\n",
		    ch->avisos_scroll);
	if (ch->ramas_saltadas)
		LOG("  AVISO: %d secciones de partitura bifurcada saltadas\n",
		    ch->ramas_saltadas);
	if (ch->n_rodillos)
		LOG("  %d rodillos/globos\n", ch->n_rodillos);
	if (ch->avisos_rodillo)
		LOG("  AVISO: %d tramos mal formados en la chart (ver tja.c)\n",
		    ch->avisos_rodillo);
	if (ch->globos_sin_cuenta)
		LOG("  AVISO: %d globos sin numero en BALLOON: (se usan %d golpes)\n",
		    ch->globos_sin_cuenta, TJA_GOLPES_POR_DEFECTO);
	if (ch->n_desbordadas)
		LOG("  AVISO: %d notas no cupieron en MAX_NOTAS\n", ch->n_desbordadas);
	if (ch->avisos_tiempo)
		LOG("  AVISO: %d comandos de tiempo a mitad de compas (ver tja.c)\n",
		    ch->avisos_tiempo);

#if VOLCAR_CHART
	for (i = 0; i < n_notas; i++)
		LOG("  NOTA %d %d %d\n", i, notas[i].tiempo_ms, notas[i].tipo);
#endif
}

// Mete una entrada en la chart. "fin" solo lo usan rodillos y globos; con 0
// se queda en una nota puntual.
static void anadir_nota(int t, int tipo, int grande, int fin, int golpes)
{
	if (n_notas >= MAX_NOTAS) return;
	notas[n_notas].scroll    = 1.0f;   // las generadas van a velocidad normal
	notas[n_notas].gogo      = 0;
	notas[n_notas].tiempo_ms = t;
	notas[n_notas].fin_ms    = (fin > t) ? fin : t;
	notas[n_notas].tipo      = tipo;
	notas[n_notas].grande    = grande;
	notas[n_notas].golpes    = golpes;
	notas[n_notas].dados     = 0;
	notas[n_notas].resuelta  = 0;
	n_notas++;
}

static void generar_chart(int dur_ms)
{
	ventana_dibujo_ms = 2200;
	float ms_por_pulso = 60000.0f / BPM_CHART;
	int pulso = PULSO_INICIAL;

	n_notas = 0;
	for (;;) {
		int t = (int)(pulso * ms_por_pulso);
		if (t > dur_ms - 500 || n_notas >= MAX_NOTAS) break;
		anadir_nota(t, (pulso % 2 == 0) ? NOTA_DON : NOTA_KA, 0, 0, 0);
		pulso++;
	}
	LOG("Chart generada: %d notas a %d BPM\n", n_notas, (int)BPM_CHART);
}

// Chart de prueba: un grupo por cada cosa que sabe hacer el motor, en orden y
// separados por silencios para poder mirarlos de uno en uno.
//
// No es para jugar, es para VER. Si algo esta mal —una nota grande que no se
// distingue, un rodillo que no suma, un globo que no revienta— aqui se ve en
// treinta segundos y sin depender de que la chart de una cancion real traiga
// ese caso pronto.
static void generar_chart_prueba(int dur_ms)
{
	ventana_dibujo_ms = 2200;
	const float pulso = 60000.0f / BPM_CHART;   // 428 ms a 140 BPM
	float t = pulso * PULSO_INICIAL;            // entrada antes de la primera
	int i;

	n_notas = 0;

	// 1) cuatro don
	for (i = 0; i < 4; i++) { anadir_nota((int)t, NOTA_DON, 0, 0, 0); t += pulso; }
	t += pulso * 2.0f;

	// 2) cuatro ka
	for (i = 0; i < 4; i++) { anadir_nota((int)t, NOTA_KA, 0, 0, 0); t += pulso; }
	t += pulso * 2.0f;

	// 3) alternando a medio pulso, que es donde se nota si el reloj patina
	for (i = 0; i < 8; i++) {
		anadir_nota((int)t, (i & 1) ? NOTA_KA : NOTA_DON, 0, 0, 0);
		t += pulso * 0.5f;
	}
	t += pulso * 2.0f;

	// 4) dos DON grandes, separadas para poder compararlas con las normales
	for (i = 0; i < 2; i++) { anadir_nota((int)t, NOTA_DON, 1, 0, 0); t += pulso * 2.0f; }

	// 5) dos KA grandes
	for (i = 0; i < 2; i++) { anadir_nota((int)t, NOTA_KA, 1, 0, 0); t += pulso * 2.0f; }
	t += pulso * 2.0f;

	// 6) rodillo corto
	anadir_nota((int)t, NOTA_RODILLO, 0, (int)(t + pulso * 2.0f), 0);
	t += pulso * 4.0f;

	// 7) rodillo largo
	anadir_nota((int)t, NOTA_RODILLO, 0, (int)(t + pulso * 4.0f), 0);
	t += pulso * 6.0f;

	// 8) rodillo GRANDE (el 6 del .tja): barra mas gorda
	anadir_nota((int)t, NOTA_RODILLO, 1, (int)(t + pulso * 4.0f), 0);
	t += pulso * 6.0f;

	// 9) globo facil: 5 golpes en 3 pulsos
	anadir_nota((int)t, NOTA_GLOBO, 0, (int)(t + pulso * 3.0f), 5);
	t += pulso * 5.0f;

	// 10) globo apretado: 10 golpes en 5 pulsos
	anadir_nota((int)t, NOTA_GLOBO, 0, (int)(t + pulso * 5.0f), 10);
	t += pulso * 7.0f;

	// 11) todo mezclado, que es como viene en una cancion de verdad
	anadir_nota((int)t, NOTA_DON, 0, 0, 0);  t += pulso;
	anadir_nota((int)t, NOTA_KA,  0, 0, 0);  t += pulso;
	anadir_nota((int)t, NOTA_DON, 1, 0, 0);  t += pulso * 2.0f;
	anadir_nota((int)t, NOTA_KA,  1, 0, 0);  t += pulso * 2.0f;
	anadir_nota((int)t, NOTA_RODILLO, 0, (int)(t + pulso * 2.0f), 0);
	t += pulso * 4.0f;
	anadir_nota((int)t, NOTA_GLOBO, 0, (int)(t + pulso * 3.0f), 6);
	t += pulso * 4.0f;

	LOG("Chart de prueba: %d entradas, acaba en %d ms\n", n_notas, (int)t);
	if ((int)t > dur_ms)
		LOG("  AVISO: la chart (%d ms) pasa de lo que dura el audio (%d ms)\n",
		    (int)t, dur_ms);
}

//---------------------------------------------------------------------
// GS (igual que en Sounds/)
//---------------------------------------------------------------------
static void init_gs(framebuffer_t *frame, zbuffer_t *z)
{
	frame->width  = 640;
	frame->height = 512;
	frame->mask   = 0;
	frame->psm    = GS_PSM_32;
	frame->address = graph_vram_allocate(frame->width, frame->height,
	                                     frame->psm, GRAPH_ALIGN_PAGE);

	z->enable  = DRAW_ENABLE;
	z->mask    = 0;
	z->method  = ZTEST_METHOD_GREATER_EQUAL;
	z->zsm     = GS_ZBUF_32;
	z->address = graph_vram_allocate(frame->width, frame->height,
	                                 z->zsm, GRAPH_ALIGN_PAGE);

	graph_initialize(frame->address, frame->width, frame->height,
	                 frame->psm, 0, 0);
}

static void init_drawing_environment(framebuffer_t *frame, zbuffer_t *z)
{
	packet_t *packet = packet_init(16, PACKET_NORMAL);
	qword_t *q = packet->data;

	q = draw_setup_environment(q, 0, frame, z);
	q = draw_primitive_xyoffset(q, 0, (2048 - 320), (2048 - 256));
	q = draw_finish(q);

	dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data,
	                        q - packet->data, 0, 0);
	dma_wait_fast();
	packet_free(packet);
}

// Circulo perfecto, sin temblor. Ya no lo usa nadie: las notas, el globo y
// las tapas del rodillo pasaron a vertices_disco/relleno, que tiemblan. Se
// queda por si hay que volver atras, y marcada para que no salte el aviso de
// funcion sin usar en cada compilacion (un aviso que siempre esta acaba
// tapando a uno que si importa).
__attribute__((unused))
static qword_t *build_circle(qword_t *q, float cx, float cy, float radius,
                             color_t color, int segments, int type)
{
	vertex_f_t puntos[segments + 2];
	xyz_t verts_fx[segments + 2];
	float escala = 1.0f / 2048.0f;
	prim_t prim;
	int i, start_idx, vert_count;

	puntos[0].x = cx * escala;
	puntos[0].y = cy * escala;
	puntos[0].z = 0.0f;
	puntos[0].w = 1.0f;

	for (i = 0; i < segments; i++) {
		float angulo = i * ((2 * M_PI) / segments);
		puntos[i + 1].x = (cx + radius * cosf(angulo)) * escala;
		puntos[i + 1].y = (cy + radius * sinf(angulo)) * escala;
		puntos[i + 1].z = 0.0f;
		puntos[i + 1].w = 1.0f;
	}
	puntos[segments + 1] = puntos[1];

	draw_convert_xyz(verts_fx, 2048, 2048, 32, segments + 2, puntos);

	prim.type         = type;
	prim.shading      = PRIM_SHADE_FLAT;
	prim.mapping      = DRAW_DISABLE;
	prim.fogging      = DRAW_DISABLE;
	prim.blending     = DRAW_DISABLE;
	prim.antialiasing = DRAW_ENABLE;
	prim.mapping_type = PRIM_MAP_ST;
	prim.colorfix     = PRIM_UNFIXED;

	q = draw_prim_start(q, 0, &prim, &color);

	start_idx  = (type == PRIM_TRIANGLE_FAN) ? 0 : 1;
	vert_count = (type == PRIM_TRIANGLE_FAN) ? segments + 2 : segments + 1;

	for (i = 0; i < vert_count; i++) {
		q->dw[0] = color.rgbaq;
		q->dw[1] = verts_fx[start_idx + i].xyz;
		q++;
	}

	return draw_prim_end(q, 2, DRAW_RGBAQ_REGLIST);
}

static qword_t *build_rect(qword_t *q, float x, float y, float w, float h,
                           color_t color)
{
	vertex_f_t puntos[4];
	xyz_t verts[4];
	float e = 1.0f / 2048.0f;
	prim_t prim;
	int i;

	puntos[0].x = x * e;       puntos[0].y = y * e;
	puntos[1].x = (x + w) * e; puntos[1].y = y * e;
	puntos[2].x = x * e;       puntos[2].y = (y + h) * e;
	puntos[3].x = (x + w) * e; puntos[3].y = (y + h) * e;
	for (i = 0; i < 4; i++) { puntos[i].z = 0.0f; puntos[i].w = 1.0f; }

	draw_convert_xyz(verts, 2048, 2048, 32, 4, puntos);

	prim.type         = PRIM_TRIANGLE_STRIP;
	prim.shading      = PRIM_SHADE_FLAT;
	prim.mapping      = DRAW_DISABLE;
	prim.fogging      = DRAW_DISABLE;
	prim.blending     = DRAW_DISABLE;
	prim.antialiasing = DRAW_DISABLE;
	prim.mapping_type = PRIM_MAP_ST;
	prim.colorfix     = PRIM_UNFIXED;

	q = draw_prim_start(q, 0, &prim, &color);
	for (i = 0; i < 4; i++) {
		q->dw[0] = color.rgbaq;
		q->dw[1] = verts[i].xyz;
		q++;
	}
	return draw_prim_end(q, 2, DRAW_RGBAQ_REGLIST);
}

//---------------------------------------------------------------------
// Texto
//
// libfont (-lfont) lee las fuentes de la BIOS: no hay que empotrar ninguna
// textura. fontx_print_ascii devuelve qword_t*, o sea que escribe en NUESTRO
// packet y convive con build_circle en la misma cadena DMA. Comprobado y
// medido en TESTS_REALES/Texto: ~8 qwords y ~6 us por caracter.
//
// El envoltorio existe para arreglar dos incompatibilidades con
// draw_convert_xyz, que son las que hacen que texto y figuras no cuadren:
//
// 1. La Y. draw_convert_xyz hace  output.y = (short)((v.y + 1.0f) * -center_y),
//    con el signo negado y contando con que el short desborde: sale
//    pantalla_y = 256 - cy, o sea que en build_circle la Y crece HACIA
//    ARRIBA. fontx hace ftoi4(v0->y) + 32768, que crece hacia abajo. Aqui se
//    niega para que todo el motor hable el idioma de las notas.
//
// 2. La Z. Las figuras salen en (v.z + 1.0f) * (1 << 31) = 2147483648 y el
//    ejemplo de la SDK pone v0.z = 4. Con el ztest en GREATER_EQUAL el texto
//    perderia contra cualquier figura ya dibujada. Se le da la Z maxima.
//
// De ahi sale una regla que hay que respetar a mano: EL TEXTO VA SIEMPRE AL
// FINAL DE LA CADENA. Cualquier figura mandada despues, con Z menor, fallaria
// el ztest y no se dibujaria, sin avisar de nada.
//---------------------------------------------------------------------
#define Z_TEXTO  0xFFFFFFFFu

//---------------------------------------------------------------------
// Texto japones
//---------------------------------------------------------------------
// La BIOS de la PS2 trae las fuentes japonesas: rom0:KROM se carga DOS veces,
// una en SINGLE_BYTE (ASCII/JIS X 0201) y otra en DOUBLE_BYTE (kana, simbolos
// y los 2965 kanji de JIS nivel 1). fontx_print_sjis mezcla las dos en la
// misma cadena el solo.
//
// El nivel 2 (kanji raros) NO esta en la consola. El fallo es benigno:
// fontx_get_char devuelve NULL, el caracter se salta y deja el hueco. No pinta
// basura.
//
// Los .tja vienen en UTF-8 (los modernos, con BOM) o en Shift-JIS (los
// antiguos). Como fontx_print_sjis quiere Shift-JIS, los titulos se convierten
// UNA vez, al catalogar, y ya se guardan convertidos.
// No esta en font.h, pero la libreria la exporta igual. Se declara aqui para
// poder comprobar que el font montado a mano indexa donde debe.
extern char *fontx_get_char(fontx_t *fontx, unsigned short c);

static fontx_t krom;
static fontx_t krom_kanji;

// 0 = no se pudo montar el font de kanji y se dibuja solo con el ASCII. NUNCA
// se cuelga por esto: sin kanji el juego funciona igual, con los titulos
// japoneses hechos un cristo, y eso es infinitamente mejor que una pantalla
// congelada.
static int hay_kanji = 0;

// Monta a mano el font de dos bytes de KROM, porque el fontx_load(DOUBLE_BYTE)
// de ps2sdk NO funciona. El fallo esta en su fontx_load_double_krom:
//
//   fontx_header->type      = DOUBLE_BYTE;      // struct: byte 18
//   fontx_header->table_num = table_num;        // struct: byte 19
//   memcpy(fontx->font + 18, sjis_table, 204);  // <- pisa los dos
//
// La cabecera FONTX2 del formato ocupa 17 bytes (18 con el numero de tablas),
// pero la struct de la libreria mete un byte de mas en el identificador y otro
// en el nombre para los terminadores, asi que sus campos caen en 16, 17, 18 y
// 19. La libreria escribe la tabla de rangos donde dice el FORMATO (byte 18) y
// luego la lee donde dice la STRUCT (byte 20): dos bytes de desfase, y de paso
// el memcpy se lleva por delante el "type". Justo despues, fontx_load compara
// ese type con el que le pediste, no cuadra, suelta "Type mismatch" y devuelve
// -1.
//
// O sea que ese camino no puede funcionar para nadie. Aqui se monta el buffer
// como lo LEE fontx_get_char, que es lo unico que importa.
//
// Devuelve 0 si salio bien.
static int cargar_krom_kanji(fontx_t *f, int wmargin, int hmargin, int bold)
{
	// Como lo lee la libreria: id[7] nombre[9] ancho alto tipo n_tablas, y los
	// pares (principio, fin) a partir del byte 20.
	const int off_ancho = 16, off_alto = 17, off_tipo = 18, off_ntab = 19;
	const int off_rangos = 20;
	const int alto = 15, ancho = 16;
	const int bytes_char = 30;      // ((16+7)>>3) * 15
	const int n_chars = 3489;

	int off_datos = off_rangos + krom_rangos_n * 4;
	int total = off_datos + n_chars * bytes_char;
	int fd, leidos = 0;
	unsigned char *b;

	f->font = NULL;

	b = (unsigned char *)malloc(total);
	if (b == NULL) {
		LOG("kanji: sin memoria para %d bytes\n", total);
		return -1;
	}
	memset(b, 0, total);

	fd = open("rom0:KROM", O_RDONLY);
	if (fd < 0) {
		LOG("kanji: no se pudo abrir rom0:KROM\n");
		free(b);
		return -1;
	}

	// Los glifos de kanji estan al PRINCIPIO de KROM; los de ASCII vienen
	// justo detras, en 0x198DE, que es exactamente 3489 * 30. Encajan.
	lseek(fd, 0, SEEK_SET);

	// A trozos y no de una: son 102 KB y el unico read grande que hace este
	// proyecto (el del .ogg) ya dio problemas por la via de fileXio. Con
	// trozos de 8 KB se sabe ademas por donde se rompio, si se rompe.
	while (leidos < n_chars * bytes_char) {
		int pedir = n_chars * bytes_char - leidos;
		int r;

		if (pedir > 8192) pedir = 8192;
		r = read(fd, b + off_datos + leidos, pedir);
		if (r <= 0) {
			LOG("kanji: read fallo en %d de %d (devolvio %d)\n",
			    leidos, n_chars * bytes_char, r);
			close(fd);
			free(b);
			return -1;
		}
		leidos += r;
	}
	close(fd);

	// La cabecera, tal cual la espera fontx_get_char.
	memcpy(b, "FONTX2", 6);
	memcpy(b + 7, "KROM", 4);
	b[off_ancho] = (unsigned char)ancho;
	b[off_alto]  = (unsigned char)alto;
	b[off_tipo]  = DOUBLE_BYTE;
	b[off_ntab]  = (unsigned char)krom_rangos_n;

	// Y los rangos donde de verdad los busca: byte 20, no 18.
	memcpy(b + off_rangos, krom_rangos,
	       (size_t)krom_rangos_n * 2 * sizeof(unsigned short));

	f->font    = (char *)b;
	f->rowsize = (ancho + 7) >> 3;
	f->charsize = f->rowsize * alto;
	f->offset   = off_datos;
	f->w_margin = (char)wmargin;
	f->h_margin = (char)hmargin;
	f->bold     = (char)bold;
	snprintf(f->name, sizeof(f->name), "KROM");

	// Una comprobacion barata de que el montaje cuadra: el primer caracter de
	// la tabla es 0x8140, el espacio ideografico, y tiene que caer justo al
	// principio de los datos.
	if (fontx_get_char(f, 0x8140) != (char *)(b + off_datos)) {
		LOG("kanji: el indice no cuadra, se sigue sin kanji\n");
		free(b);
		f->font = NULL;
		return -1;
	}

	LOG("kanji: %d glifos, %d bytes\n", n_chars, total);
	return 0;
}

//---------------------------------------------------------------------
// Lineas con temblor
//---------------------------------------------------------------------
static void iniciar_ruido(void)
{
	int i;
	// Semilla fija a proposito: si algo se ve raro, se ve raro IGUAL en cada
	// arranque y se puede perseguir.
	srand(1234);
	for (i = 0; i < RUIDO_N; i++)
		ruido_tabla[i] = (float)rand() / (float)RAND_MAX * 2.0f - 1.0f;
}

// Ruido continuo: interpola entre dos valores de la tabla con una curva
// suave (3t^2-2t^3), que es lo que evita los escalones.
static float ruido(int semilla, float t)
{
	int i = (int)t;
	float f = t - (float)i;
	float a = ruido_tabla[(semilla + i) & RUIDO_MASCARA];
	float b = ruido_tabla[(semilla + i + 1) & RUIDO_MASCARA];

	f = f * f * (3.0f - 2.0f * f);
	return a + (b - a) * f;
}

// Reloj del temblor: se muestrea UNA VEZ POR FOTOGRAMA ENTERO, no por vsync.
//
// En modo entrelazado cada vsync es un CAMPO, o sea medio fotograma. Si el
// ruido cambia en cada vsync, los dos campos de un mismo fotograma llevan
// geometria DISTINTA, y entonces:
//
//   - en un emulador que mezcla campos (PCSX2 con desentrelazado automatico)
//     cada trazo sale con una copia tenue desplazada: la figura "doble"
//   - en un tubo de verdad los campos no se mezclan, se alternan a 50 Hz, y
//     se ve como un parpadeo del contorno
//
// Comprobado en PCSX2 con dos ISOs identicas salvo el temblor: con el temblor
// a cero los trazos salen limpios, con temblor por campo salen dobles, y
// muestreando por fotograma vuelven a salir limpios con el temblor vivo.
//
// El bit 13 del CSR del GS dice que campo se esta mostrando (0 par, 1 impar).
// Se toma la muestra al entrar en el par y se reutiliza en el impar, asi que
// el temblor se refresca a 25 Hz: para un ruido de periodo 180 ms sobra.
//
// El tiempo se ACUMULA en vez de leer cpu_ticks a pelo porque ese contador es
// de 32 bits y da la vuelta cada ~14,5 s; la resta sin signo sale bien aunque
// haya dado la vuelta mientras el intervalo sea corto, que es el mismo truco
// que usa el reloj de cancion.
static float reloj_temblor(void)
{
	static unsigned int ticks_prev = 0;
	static float ms = 0.0f;
	static float t_fijo = -1.0f;
	static int campo_prev = -1;
	unsigned int ahora_t = cpu_ticks();
	int campo;

	if (ticks_prev != 0)
		ms += (float)(unsigned int)(ahora_t - ticks_prev)
		      * 1000.0f / (float)TICKS_POR_SEG;
	ticks_prev = ahora_t;

	campo = (int)((*GS_REG_CSR >> 13) & 1);
	if (t_fijo < 0.0f || (campo == 0 && campo != campo_prev))
		t_fijo = ms / RUIDO_PERIODO_MS;
	campo_prev = campo;

	return t_fijo;
}

// Una polilinea con los vertices temblando: la primitiva de la que sale todo
// lo demas. Cada vertice coge su propia semilla, asi que se mueven
// desacompasados y la figura parece trazada a mano en vez de sacudida.
static qword_t *poli(qword_t *q, const float *px, const float *py, int n,
                     int cerrada, color_t color, int semilla, float t)
{
	// 160 y no 64: la onda del alma pasa de noventa puntos con el diente a 15
	// grados, y como ahora el ancho varia, en el peor caso (todos estrechos)
	// se acerca a 130. Son 160*(16+8) = ~4 KB de pila, que sobra.
	vertex_f_t puntos[160];
	xyz_t verts[160];
	float escala = 1.0f / 2048.0f;
	prim_t prim;
	int i, total;

	if (n < 2) return q;
	if (n > 158) n = 158;
	total = cerrada ? n + 1 : n;

	for (i = 0; i < n; i++) {
		float dx = ruido(semilla + i * 2,     t) * amp_temblor;
		float dy = ruido(semilla + i * 2 + 1, t) * amp_temblor;

		puntos[i].x = (px[i] + dx) * escala;
		puntos[i].y = (py[i] + dy) * escala;
		puntos[i].z = 0.0f;
		puntos[i].w = 1.0f;
	}
	if (cerrada) puntos[n] = puntos[0];

	draw_convert_xyz(verts, 2048, 2048, 32, total, puntos);

	prim.type         = PRIM_LINE_STRIP;
	prim.shading      = PRIM_SHADE_FLAT;
	prim.mapping      = DRAW_DISABLE;
	prim.fogging      = DRAW_DISABLE;
	prim.blending     = DRAW_DISABLE;
	prim.antialiasing = DRAW_ENABLE;
	prim.mapping_type = PRIM_MAP_ST;
	prim.colorfix     = PRIM_UNFIXED;

	q = draw_prim_start(q, 0, &prim, &color);
	for (i = 0; i < total; i++) {
		q->dw[0] = color.rgbaq;
		q->dw[1] = verts[i].xyz;
		q++;
	}
	// draw_prim_end cierra el GIFtag y cuadra el paquete. Sin esto no sale un
	// dibujo torcido: sale basura o un cuelgue.
	return draw_prim_end(q, 2, DRAW_RGBAQ_REGLIST);
}

static qword_t *aro(qword_t *q, float cx, float cy, float r, int lados,
                    color_t color, int semilla, float t)
{
	float px[64], py[64];
	int i;

	if (lados > 60) lados = 60;
	for (i = 0; i < lados; i++) {
		float a = (float)i * (2.0f * (float)M_PI) / (float)lados;
		px[i] = cx + r * cosf(a);
		py[i] = cy + r * sinf(a);
	}
	return poli(q, px, py, lados, 1, color, semilla, t);
}

static qword_t *linea(qword_t *q, float x0, float y0, float x1, float y1,
                      color_t color, int semilla, float t)
{
	float px[2], py[2];
	px[0] = x0; py[0] = y0;
	px[1] = x1; py[1] = y1;
	return poli(q, px, py, 2, 0, color, semilla, t);
}

// Vertices de un poligono que tiembla Y GIRA despacio.
//
// El problema de poner el ruido solo en el contorno es que se ve poco, y al
// subirlo el aro se despega del relleno y se vuelve loco por su cuenta. Con
// esto tiembla la figura ENTERA: el relleno y su contorno salen de los mismos
// vertices, asi que no se pueden desincronizar por mucho que se suba.
//
// Y encima gira un poco. Un poligono de 14 lados es casi un circulo y el
// temblor solo se nota en el borde; girandolo despacio se mueven las facetas,
// que es lo que hace que no parezca un sello estampado.
static int vertices_disco(float *px, float *py, float cx, float cy, float r,
                          int lados, int semilla, float t)
{
	// Giro lento y corto: la faceta de un poligono de 14 lados mide 25 grados,
	// asi que con +-0,2 rad (11 grados) se mueve casi media faceta. Mas seria
	// verlo girar, y esto no gira: tiembla.
	float giro = ruido(semilla + 500, t * 0.35f) * 0.2f;
	int i;

	if (lados > 60) lados = 60;
	for (i = 0; i < lados; i++) {
		float a = giro + (float)i * (2.0f * (float)M_PI) / (float)lados;
		float rr = r + ruido(semilla + i, t) * amp_temblor;

		px[i] = cx + rr * cosf(a);
		py[i] = cy + rr * sinf(a);
	}
	return lados;
}

// Relleno a partir de vertices YA calculados: no les anade ruido, porque ya
// lo traen. Es lo que evita el doble temblor.
static qword_t *relleno(qword_t *q, const float *px, const float *py, int n,
                        float cx, float cy, color_t color)
{
	vertex_f_t puntos[66];
	xyz_t verts[66];
	float escala = 1.0f / 2048.0f;
	prim_t prim;
	int i;

	if (n < 3) return q;
	if (n > 60) n = 60;

	puntos[0].x = cx * escala;
	puntos[0].y = cy * escala;
	puntos[0].z = 0.0f;
	puntos[0].w = 1.0f;
	for (i = 0; i < n; i++) {
		puntos[i + 1].x = px[i] * escala;
		puntos[i + 1].y = py[i] * escala;
		puntos[i + 1].z = 0.0f;
		puntos[i + 1].w = 1.0f;
	}
	puntos[n + 1] = puntos[1];   // cierra el abanico

	draw_convert_xyz(verts, 2048, 2048, 32, n + 2, puntos);

	prim.type         = PRIM_TRIANGLE_FAN;
	prim.shading      = PRIM_SHADE_FLAT;
	prim.mapping      = DRAW_DISABLE;
	prim.fogging      = DRAW_DISABLE;
	prim.blending     = DRAW_DISABLE;
	prim.antialiasing = DRAW_ENABLE;
	prim.mapping_type = PRIM_MAP_ST;
	prim.colorfix     = PRIM_UNFIXED;

	q = draw_prim_start(q, 0, &prim, &color);
	for (i = 0; i < n + 2; i++) {
		q->dw[0] = color.rgbaq;
		q->dw[1] = verts[i].xyz;
		q++;
	}
	return draw_prim_end(q, 2, DRAW_RGBAQ_REGLIST);
}

// Contorno a partir de vertices ya calculados, por lo mismo.
static qword_t *trazo(qword_t *q, const float *px, const float *py, int n,
                      color_t color)
{
	vertex_f_t puntos[66];
	xyz_t verts[66];
	float escala = 1.0f / 2048.0f;
	prim_t prim;
	int i;

	if (n < 2) return q;
	if (n > 64) n = 64;

	for (i = 0; i < n; i++) {
		puntos[i].x = px[i] * escala;
		puntos[i].y = py[i] * escala;
		puntos[i].z = 0.0f;
		puntos[i].w = 1.0f;
	}
	puntos[n] = puntos[0];

	draw_convert_xyz(verts, 2048, 2048, 32, n + 1, puntos);

	prim.type         = PRIM_LINE_STRIP;
	prim.shading      = PRIM_SHADE_FLAT;
	prim.mapping      = DRAW_DISABLE;
	prim.fogging      = DRAW_DISABLE;
	prim.blending     = DRAW_DISABLE;
	prim.antialiasing = DRAW_ENABLE;
	prim.mapping_type = PRIM_MAP_ST;
	prim.colorfix     = PRIM_UNFIXED;

	q = draw_prim_start(q, 0, &prim, &color);
	for (i = 0; i < n + 1; i++) {
		q->dw[0] = color.rgbaq;
		q->dw[1] = verts[i].xyz;
		q++;
	}
	return draw_prim_end(q, 2, DRAW_RGBAQ_REGLIST);
}

// Lineas verticales cayendo: el aviso de nota mala.
//
// Es lo contrario del destello a proposito. El destello sale del centro hacia
// TODOS lados y celebra; esto son trazos verticales que se descuelgan hacia
// abajo y se apagan. Misma gramatica (solo lineas rectas), lectura opuesta:
// no hay que mirar el color para saber que ha ido mal.
static qword_t *desanimo(qword_t *q, float x, float y, float prog, color_t c,
                         int semilla, float t)
{
	float caida = 26.0f * prog;             // se van escurriendo hacia abajo
	float largo = 30.0f * (1.0f - prog);    // y encogiendo hasta desaparecer
	int i;

	if (largo < 1.0f) return q;

	// Cuelgan de la PARTE DE ARRIBA del juez. El aro tiene 34 de radio: a 70
	// quedaban despegadas por completo y a 20 caian en el centro tapando la
	// nota. Naciendo a 44 asoman justo por encima del borde y bajan sobre el.
	for (i = 0; i < 5; i++) {
		float dx = (float)(i - 2) * 13.0f;
		float y0 = y + 44.0f - caida;

		q = linea(q, x + dx, y0, x + dx, y0 - largo, c, semilla + i * 5, t);
	}
	return q;
}

// Trazos rectos saliendo hacia fuera: la unica forma de "efecto" que admite
// este estilo. Sin particulas, sin destellos redondos, solo lineas.
//
// prog va de 0 (recien golpeado) a 1 (se acabo): los rayos se alejan del
// centro y se acortan, asi que el destello se abre y se apaga solo.
// Alto de cada diente de la onda del alma. Sale de la tabla del ruido y es
// FIJO por diente (no depende del tiempo): asi la onda tiene una silueta
// irregular y estable, como un sismografo, y lo que se mueve encima es el
// temblor de poli(). Con todos los dientes iguales parecia lo que era, una
// onda triangular generada, y cantaba.
static float alto_diente(int k)
{
	float v = ruido_tabla[(k * 7) & RUIDO_MASCARA];

	if (v < 0.0f) v = -v;
	// Practicamente el maximo, bajando unos pocos pixeles. La variacion esta
	// para que no parezca generado, no para que la onda se encoja: con el
	// suelo bajo se desinflaba y perdia la sensacion de barra.
	return ALMA_AMP * (0.86f + 0.14f * v);
}

// Ancho de cada diente, tambien irregular.
//
// Esto es lo que rompe los rombos: con el ancho fijo, las dos hebras ponian
// sus vertices en las MISMAS x y al cruzarse dibujaban una reja de rombos
// perfectos, que es justo lo contrario de lo que se busca. Con el ancho
// variando por diente y por hebra, cada una lleva su propio paso, los
// vertices no coinciden nunca y los cruces caen donde toque.
//
// Y como el angulo de un diente sale de su alto y su ancho, variar los dos
// significa que no hay dos angulos iguales en toda la onda.
static float ancho_diente(int k, int semilla)
{
	float v = ruido_tabla[(k * 13 + 5 + semilla) & RUIDO_MASCARA];

	if (v < 0.0f) v = -v;
	return DIENTE_ALMA * (0.72f + 0.56f * v);
}

// Dibuja una onda del alma a partir de una lista de x, alternando arriba y
// abajo, cortada exactamente donde llega el alma.
//
// La punta va INTERPOLADA hasta ese corte. Antes se clavaba en el centro de
// la banda y por eso el avance parecia a saltos: la linea se quedaba quieta
// mientras crecia el trozo que faltaba para el vertice siguiente, y al llegar
// aparecia el pico entero de golpe.
static qword_t *onda_alma(qword_t *q, const float *xs, int nxs, int invertida,
                          int desp, float fin_x, color_t c, int semilla,
                          float t)
{
	float px[160], py[160];
	int n = 0, i;

	for (i = 0; i < nxs && n < 158; i++) {
		float h = alto_diente(i + desp);

		if (xs[i] > fin_x) break;
		px[n] = xs[i];
		py[n] = ALMA_Y + (((i & 1) ^ invertida) ? -h : h);
		n++;
	}

	if (n >= 1 && i < nxs && fin_x > px[n - 1] && n < 159) {
		float h  = alto_diente(i + desp);
		float yb = ALMA_Y + (((i & 1) ^ invertida) ? -h : h);
		float xa = px[n - 1], ya = py[n - 1], xb = xs[i];
		float f  = (xb > xa) ? (fin_x - xa) / (xb - xa) : 0.0f;

		px[n] = fin_x;
		py[n] = ya + (yb - ya) * f;
		n++;
	}

	if (n >= 2) return poli(q, px, py, n, 0, c, semilla, t);
	return q;
}

static qword_t *destello(qword_t *q, float x, float y, int rayos,
                         float prog, color_t c, int semilla, float t)
{
	float dentro = 14.0f + 30.0f * prog;
	float largo  = 20.0f * (1.0f - prog);
	int i;

	if (largo < 1.0f) return q;

	for (i = 0; i < rayos; i++) {
		float a = (float)i * (2.0f * (float)M_PI) / (float)rayos;
		float ca = cosf(a), sa = sinf(a);
		q = linea(q, x + dentro * ca, y + dentro * sa,
		          x + (dentro + largo) * ca, y + (dentro + largo) * sa,
		          c, semilla + i * 3, t);
	}
	return q;
}

// Ancho aproximado de una cadena, para poder centrarla. Un caracter de dos
// bytes de Shift-JIS ocupa el doble que uno ASCII, asi que contar bytes no
// vale: un titulo japones saldria descentrado justo el doble de lo que mide.
#define ANCHO_CAR  8.4f

static float ancho_texto(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	float w = 0.0f;

	while (*p) {
		if (((*p >= 0x81 && *p <= 0x9F) || (*p >= 0xE0 && *p <= 0xFC)) && p[1]) {
			w += ANCHO_CAR * 2.0f;
			p += 2;
		} else {
			w += ANCHO_CAR;
			p++;
		}
	}
	return w;
}

static qword_t *texto(qword_t *q, float x, float y, const char *s, color_t c)
{
	vertex_t v0;
	v0.x = x;
	v0.y = -y;
	v0.z = Z_TEXTO;
	// print_sjis y no print_ascii: con las dos fuentes puestas se traga
	// cualquier cadena, y para una de solo ASCII hace exactamente lo mismo
	// que la otra (mismos margenes, mismo x_orig en LEFT_ALIGN).
	//
	// Si el font de kanji no se pudo montar se tira del ASCII a secas: los
	// titulos japoneses saldran ilegibles, pero el juego funciona.
	if (hay_kanji)
		return fontx_print_sjis(q, 0, (const unsigned char *)s,
		                        LEFT_ALIGN, &v0, &c, &krom, &krom_kanji);

	return fontx_print_ascii(q, 0, (const unsigned char *)s,
	                         LEFT_ALIGN, &v0, &c, &krom);
}

static qword_t *texto_centrado(qword_t *q, float y, const char *s, color_t c)
{
	return texto(q, -ancho_texto(s) * 0.5f, y, s, c);
}

// Centrado sobre una x cualquiera, no sobre el medio de la pantalla.
static qword_t *texto_centrado_en(qword_t *q, float cx, float y,
                                  const char *s, color_t c)
{
	return texto(q, cx - ancho_texto(s) * 0.5f, y, s, c);
}

// Pegado por la derecha a una x. Hace falta para que una columna de numeros
// quede cuadrada: alineados por la izquierda, "50" y "400" no empiezan en el
// mismo sitio y la columna baila.
static qword_t *texto_derecha(qword_t *q, float x_fin, float y,
                              const char *s, color_t c)
{
	return texto(q, x_fin - ancho_texto(s), y, s, c);
}

//---------------------------------------------------------------------
// Bucle de juego
//---------------------------------------------------------------------

// Busca la nota sin resolver mas cercana en el tiempo del tipo pedido.
// Devuelve el indice o -1.
// Rodillo o globo abierto ahora mismo, o -1. Solo puede haber uno: el parser
// cierra el tramo anterior antes de abrir otro.
//
// Empieza en "desde" (el cursor de notas vivas) para no recorrer la chart
// entera en cada frame, y corta en cuanto encuentra una entrada que todavia
// no ha empezado: el array esta ordenado por tiempo de inicio.
static int tramo_activo(int desde, int ahora_ms)
{
	int i;

	for (i = desde; i < n_notas; i++) {
		int inicio;

		// El corte se hace con el adelanto puesto: las notas van en orden, y
		// si la que viene no ha abierto ni contando los 17 ms del globo,
		// ninguna de las siguientes lo habra hecho.
		if (notas[i].tiempo_ms - ADELANTO_GLOBO_MS > ahora_ms) break;
		if (notas[i].resuelta) continue;
		if (notas[i].tipo != NOTA_RODILLO && notas[i].tipo != NOTA_GLOBO)
			continue;

		// Y el globo, y solo el globo, se puede empezar a aporrear 17 ms
		// antes de que llegue su cabeza.
		inicio = notas[i].tiempo_ms -
		         ((notas[i].tipo == NOTA_GLOBO) ? ADELANTO_GLOBO_MS : 0);

		if (ahora_ms >= inicio && ahora_ms <= notas[i].fin_ms) return i;
	}
	return -1;
}

static int nota_mas_cercana(int tipo, int ahora_ms)
{
	int i, mejor = -1, mejor_dif = ventana_activa + 1;

	for (i = 0; i < n_notas; i++) {
		int dif;
		if (notas[i].resuelta || notas[i].tipo != tipo) continue;
		dif = notas[i].tiempo_ms - ahora_ms;
		if (dif < 0) dif = -dif;
		if (dif > ventana_activa) continue;
		if (dif < mejor_dif) { mejor_dif = dif; mejor = i; }
	}
	return mejor;
}

//---------------------------------------------------------------------
// Ajustes guardados en el pen
//---------------------------------------------------------------------
// Un fichero de texto de una linea ("offset=-12"), no una estructura
// binaria. Son veinte bytes: se puede abrir en el PC con cualquier editor
// para ver que hay o para arreglarlo a mano, y no hay que pensar en el orden
// de los bytes ni en versiones de formato. Cuando haya que guardar mas cosas
// (puntuaciones), se le añaden lineas y el que lea se salta las que no
// conozca.
//
// Va al pen y no a la memory card porque el pen ya esta montado y leyendose:
// la tarjeta necesitaria mcman/mcserv y un camino nuevo entero.

// Lo ultimo que le paso al fichero de ajustes, para poder enseñarlo. Cabe en
// 26 caracteres A PROPOSITO: en el menu va en la misma linea que el dato de
// RAM (que empieza en x=150), y con textos mas largos las dos se pisarian. Y
// el mensaje por defecto, el de "no habia fichero", es justo el que se ve la
// primera vez que alguien enciende esto.
static char config_estado[64] = "sin leer";

// 1 solo si al arrancar habia fichero Y se pudo leer. Es lo que decide si se
// enseña la pantalla de bienvenida: un fichero corrupto cuenta como que no hay
// nada configurado, porque el offset que se estaria usando seria el de fabrica
// y el jugador no tiene por que enterarse por su cuenta.
static int config_existe = 0;

// Saca "clave=NUMERO" del texto. Devuelve 1 si estaba y el numero cabia en el
// rango. Las claves que no esten se quedan como estaban: asi un fichero viejo,
// escrito por una version que solo guardaba el offset, sigue valiendo y los
// ajustes nuevos salen con su valor por defecto.
static int leer_clave(const char *texto, const char *clave, int *destino,
                      int minimo, int maximo)
{
	const char *pos = strstr(texto, clave);
	char *fin;
	long v;

	if (pos == NULL) return 0;
	pos += strlen(clave);

	// strtol y no atoi: hace falta el puntero de salida para distinguir un
	// "clave=0" de un "clave=loquesea", que atoi tambien daria como 0.
	v = strtol(pos, &fin, 10);
	if (fin == pos || v < minimo || v > maximo) return 0;

	*destino = (int)v;
	return 1;
}

static void cargar_config(void)
{
	FILE *f;
	char buf[TAM_CONFIG];
	size_t leido;

	f = fopen(RUTA_CONFIG, "rb");
	if (f == NULL) {
		snprintf(config_estado, sizeof(config_estado),
		         "sin fichero de ajustes");
		return;
	}
	leido = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[leido] = '\0';

	// El offset manda: si esa clave no esta o no se entiende, el fichero no
	// cuenta como perfil y se vuelve a preguntar al arrancar.
	if (!leer_clave(buf, "offset=", &offset_ms, -2000, 2000)) {
		snprintf(config_estado, sizeof(config_estado),
		         "fichero ilegible");
		return;
	}

	leer_clave(buf, "musica=", &vol_musica, 0, VOL_PASOS);
	leer_clave(buf, "sonido=", &vol_sonido, 0, VOL_PASOS);

	// El estilo de nota SI es del jugador (se elige en las opciones), asi que
	// vive aqui. El temblor NO: es fijo en el codigo. Un perfil viejo puede
	// traer todavia sus claves "ruido=" y "ruidomus="; se ignoran solas, que
	// para eso el formato deja pasar las claves que no conoce.
	leer_clave(buf, "estilo=", &estilo_nota, 0, 1);

	config_existe = 1;
	snprintf(config_estado, sizeof(config_estado),
	         "leidos del pen");
}

// Devuelve 0 si se guardo Y se pudo volver a leer con el mismo valor.
//
// La relectura no es paranoia: no esta comprobado que el bdmfs_fatfs de este
// ps2sdk escriba de verdad, y un fopen("wb") que devuelve un FILE* valido y
// luego no deja nada en el pen daria un "guardado" mentiroso que solo se
// descubriria al reiniciar la consola.
static int guardar_config(void)
{
	FILE *f;
	char buf[TAM_CONFIG];
	size_t largo, leido;
	int v = 0;

	f = fopen(RUTA_CONFIG, "wb");
	if (f == NULL) {
		snprintf(config_estado, sizeof(config_estado),
		         "NO se guarda: protegido");
		LOG("guardar_config: fopen(%s, wb) fallo\n", RUTA_CONFIG);
		return -1;
	}
	// Una clave por linea: asi se le pueden añadir cosas sin tocar al que lee,
	// y se puede arreglar a mano desde el PC.
	snprintf(buf, sizeof(buf), "offset=%d\nmusica=%d\nsonido=%d\nestilo=%d\n",
	         offset_ms, vol_musica, vol_sonido, estilo_nota);
	largo = strlen(buf);
	if (fwrite(buf, 1, largo, f) != largo) {
		fclose(f);
		snprintf(config_estado, sizeof(config_estado),
		         "NO se guarda: no escribe");
		LOG("guardar_config: fwrite de %d bytes fallo\n", (int)largo);
		return -1;
	}
	fclose(f);

	f = fopen(RUTA_CONFIG, "rb");
	if (f == NULL) {
		snprintf(config_estado, sizeof(config_estado),
		         "NO se guarda: no relee");
		LOG("guardar_config: no se pudo reabrir %s\n", RUTA_CONFIG);
		return -1;
	}
	leido = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[leido] = '\0';

	// Se comprueba el offset y no los tres: con que uno haya llegado bien al
	// pen, la escritura ha funcionado, que es lo que se esta comprobando.
	if (!leer_clave(buf, "offset=", &v, -2000, 2000) || v != offset_ms) {
		snprintf(config_estado, sizeof(config_estado),
		         "NO se guarda: no cuadra");
		LOG("guardar_config: releido '%s', se esperaba offset=%d\n",
		    buf, offset_ms);
		return -1;
	}

	config_existe = 1;
	snprintf(config_estado, sizeof(config_estado),
	         "guardada: %d ms", offset_ms);
	LOG("%s\n", config_estado);
	return 0;
}

// Manda los dos volumenes a audsrv. Se llama al arrancar y cada vez que se
// mueve una rayita en las opciones.
//
// Ojo con quien la llama: la regla del motor es que solo el hilo de audio toca
// audsrv mientras suena una cancion. Las opciones estan dentro del selector,
// donde ese hilo no existe, asi que desde ahi se puede llamar sin carrera. Al
// arrancar, igual.
static void aplicar_volumen_musica(void)
{
	audsrv_set_volume(MAX_VOLUME * vol_musica / VOL_PASOS);
}

// Van separadas porque esta cuesta 24 llamadas y la otra una. Cada
// audsrv_adpcm_set_volume es una ida y vuelta al IOP por SIF, y esto se llama
// desde el menu de opciones en cada pulsacion: mover la barra de la MUSICA no
// tiene por que pagar las 24 del sonido.
//
// Son 24 y no una porque los golpes salen con ch = -1 (el primer canal libre)
// y no se sabe en cual van a caer.
static void aplicar_volumen_sonido(void)
{
	int i;

	for (i = 0; i < N_CANALES_SPU; i++)
		audsrv_adpcm_set_volume(i, 100 * vol_sonido / VOL_PASOS);
}

static void aplicar_volumen(void)
{
	aplicar_volumen_musica();
	aplicar_volumen_sonido();
}

// Borra el perfil del pen y deja los ajustes como salidos de fabrica.
// Devuelve 0 si el fichero ya no esta.
//
// Se prueban las dos capas y se COMPRUEBA el resultado abriendo otra vez: no
// esta comprobado cual de las dos borra de verdad en este ps2sdk, y un borrado
// que dice que si y deja el fichero seria peor que uno que falla, porque al
// reiniciar reaparecerian los ajustes viejos sin explicacion.
static int borrar_perfil(void)
{
	FILE *f;

	remove(RUTA_CONFIG);
	f = fopen(RUTA_CONFIG, "rb");
	if (f != NULL) {
		fclose(f);
		fileXioRemove(RUTA_CONFIG);
		f = fopen(RUTA_CONFIG, "rb");
	}
	if (f != NULL) {
		fclose(f);
		snprintf(config_estado, sizeof(config_estado),
		         "NO se borra: sigue ahi");
		LOG("borrar_perfil: %s sigue existiendo\n", RUTA_CONFIG);
		return -1;
	}

	offset_ms     = OFFSET_LATENCIA_MS;
	vol_musica    = VOL_PASOS;
	vol_sonido    = VOL_PASOS;
	config_existe = 0;
	aplicar_volumen();

	snprintf(config_estado, sizeof(config_estado), "perfil borrado");
	LOG("Perfil borrado: %s\n", RUTA_CONFIG);
	return 0;
}

//---------------------------------------------------------------------
// Puntuacion
//---------------------------------------------------------------------
// Reparto "shin'uchi", que es el del taiko actual: la partitura ENTERA vale un
// millon, se le quita lo que van a dar rodillos y globos, y lo que queda se
// reparte a partes iguales entre las notas. Sale de OpenTaiko
// (GetAddScoreGen4ShinUchi en CStage演奏画面共通.cs).
//
// Se eligio este y no el viejo de SCOREINIT/SCOREDIFF por tres motivos, y los
// tres son de este proyecto y no de gustos:
//
//   1. No necesita NADA del .tja. Los tres ficheros de prueba no traen
//      SCOREINIT ni SCOREDIFF, que es lo normal.
//   2. No necesita Gogo Time (x1,2 en el sistema viejo), que no parseamos.
//   3. Da la misma escala 0..1000000 para todas las canciones, que es lo unico
//      que hace comparables las puntuaciones que se guardan.
//
// Lo que se pierde: en el sistema viejo las notas grandes valen doble y el
// combo sube el valor de la nota. En shin'uchi no, todas valen igual.
#define PUNTOS_TOTALES  1000000
#define PUNTOS_TRAMO        100   // cada golpe de rodillo o de globo

static int calcular_puntos_nota(void)
{
	int i, n = 0, globos = 0;
	long long rod_ms = 0, resto;

	for (i = 0; i < n_notas; i++) {
		if (notas[i].tipo == NOTA_DON || notas[i].tipo == NOTA_KA) n++;
		else if (notas[i].tipo == NOTA_GLOBO) globos += notas[i].golpes;
		else if (notas[i].tipo == NOTA_RODILLO)
			rod_ms += notas[i].fin_ms - notas[i].tiempo_ms;
	}

	if (n == 0) return 0;

	// 16,6 golpes por segundo es lo que el juego da por hecho que se puede
	// aporrear un rodillo; a 100 puntos el golpe, 1660 puntos por segundo.
	resto = (long long)PUNTOS_TOTALES
	        - (long long)globos * PUNTOS_TRAMO
	        - (1660LL * rod_ms) / 1000;
	if (resto < 0) resto = 0;   // una chart de puro rodillo se lo comeria todo

	// Redondeo hacia ARRIBA a decenas, igual que el original.
	return (int)(((resto + (long long)n * 10 - 1) / ((long long)n * 10)) * 10);
}

// Fichero de puntuaciones, uno por cancion y dentro de su carpeta. Va ahi y no
// en un indice central para que la puntuacion viaje con la cancion: copias la
// carpeta a otro pen y se copia con ella. Ademas, si alguien borra una carpeta
// desde el PC no queda una entrada huerfana en ningun sitio.
#define NOMBRE_PUNTOS  "PUNTOS.CFG"

static const char *clave_puntos[N_CURSOS] = {
	"facil=", "normal=", "dificil=", "oni=", "edit="
};

// Mejor marca del curso que se esta jugando, o -1. La lee main ANTES de
// arrancar el hilo de audio: leer del pen con la cancion sonando se nota.
static int mejor_puntos = -1;

// Devuelve 0 si la cancion no tiene carpeta (las generadas no la tienen).
static int ruta_puntos(const cancion_t *c, char *dst, size_t n)
{
	const char *barra;
	size_t largo;

	if (c->generada || c->ruta_tja[0] == '\0') return 0;

	barra = strrchr(c->ruta_tja, '/');
	if (barra == NULL) return 0;

	largo = (size_t)(barra - c->ruta_tja) + 1;   // la barra se queda
	if (largo + sizeof(NOMBRE_PUNTOS) > n) return 0;

	memcpy(dst, c->ruta_tja, largo);
	memcpy(dst + largo, NOMBRE_PUNTOS, sizeof(NOMBRE_PUNTOS));
	return 1;
}

// Las marcas de los cinco cursos de una sola pasada. Va asi y no con cinco
// leer_puntos() porque cada uno abriria el fichero otra vez, y en el pen abrir
// cuesta bastante mas que leer (medido: ver "pen: N KB abrir N ms leer N ms").
// Los cursos que no tengan marca se quedan a -1.
static void leer_puntos_todos(const cancion_t *c, int *v)
{
	char ruta[224], buf[TAM_CONFIG];
	FILE *f;
	size_t leido;
	int k;

	for (k = 0; k < N_CURSOS; k++) v[k] = -1;

	if (!ruta_puntos(c, ruta, sizeof(ruta))) return;

	f = fopen(ruta, "rb");
	if (f == NULL) return;
	leido = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[leido] = '\0';

	for (k = 0; k < N_CURSOS; k++)
		leer_clave(buf, clave_puntos[k], &v[k], 0, 9999999);
}

static int leer_puntos(const cancion_t *c, int curso)
{
	int v[N_CURSOS];

	leer_puntos_todos(c, v);
	return v[curso];
}

// Guarda la marca de UN curso conservando la de los demas: el fichero se
// reescribe entero, asi que primero hay que leer lo que hubiera.
static int guardar_puntos(const cancion_t *c, int curso, int puntos)
{
	char ruta[224], buf[TAM_CONFIG];
	FILE *f;
	size_t largo = 0;
	int v[N_CURSOS];
	int k;

	if (!ruta_puntos(c, ruta, sizeof(ruta))) return -1;

	// Se lee lo que hubiera para no perder los otros cursos: el fichero se
	// reescribe entero, no se le añade al final.
	leer_puntos_todos(c, v);
	v[curso] = puntos;

	buf[0] = '\0';
	for (k = 0; k < N_CURSOS; k++) {
		if (v[k] < 0) continue;
		if (largo + 24 >= sizeof(buf)) break;
		largo += (size_t)snprintf(buf + largo, sizeof(buf) - largo, "%s%d\n",
		                          clave_puntos[k], v[k]);
	}

	f = fopen(ruta, "wb");
	if (f == NULL) {
		LOG("guardar_puntos: no se pudo abrir %s\n", ruta);
		return -1;
	}
	if (fwrite(buf, 1, largo, f) != largo) {
		fclose(f);
		LOG("guardar_puntos: fallo al escribir %s\n", ruta);
		return -1;
	}
	fclose(f);

	LOG("Puntuacion guardada en %s: %s %d\n", ruta, nombre_curso[curso],
	    puntos);
	return 0;
}

// Se lo lleva por delante todo, que es lo que promete el menu de opciones:
// "Borrar perfil (y puntuaciones)". Si el fichero no estaba, remove falla y da
// igual: el resultado que se busca es que no este.
static int borrar_todas_las_puntuaciones(void)
{
	char ruta[224];
	int i, borrados = 0;

	for (i = 0; i < n_canciones; i++) {
		if (!ruta_puntos(&canciones[i], ruta, sizeof(ruta))) continue;
		remove(ruta);
		fileXioRemove(ruta);
		borrados++;
	}
	LOG("Puntuaciones borradas de %d carpetas\n", borrados);
	return borrados;
}

//---------------------------------------------------------------------
// Calibracion
//---------------------------------------------------------------------
static int cal_muestras[MAX_MUESTRAS_CAL];
static int cal_n = 0;
// Resultado de la ultima partida, para que la pantalla de resultados lo
// pueda enseñar y ofrecer guardarlo. -1 en cal_disp = no hay medida.
static int cal_mediana = 0;
static int cal_disp    = -1;

// Mediana y cuartiles del desfase. Mediana y no media a proposito: un golpe
// suelto muy fuera (un despiste) arrastra la media y no mueve la mediana. La
// dispersion p75-p25 dice si el numero es de fiar.
static void informe_calibracion(void)
{
	int i, j, tmp, p25, p50, p75;

	cal_disp = -1;
	if (cal_n < 20) {
		LOG("Calibracion: solo %d golpes, hacen falta bastantes mas.\n", cal_n);
		return;
	}

	for (i = 1; i < cal_n; i++) {          // insercion; sobra para unos cientos
		tmp = cal_muestras[i];
		for (j = i - 1; j >= 0 && cal_muestras[j] > tmp; j--)
			cal_muestras[j + 1] = cal_muestras[j];
		cal_muestras[j + 1] = tmp;
	}

	p25 = cal_muestras[cal_n / 4];
	p50 = cal_muestras[cal_n / 2];
	p75 = cal_muestras[(cal_n * 3) / 4];

	LOG("=== CALIBRACION: %d golpes ===\n", cal_n);
	LOG("  p25 %d ms | mediana %d ms | p75 %d ms | dispersion %d ms\n",
	    p25, p50, p75, p75 - p25);
	LOG("  (mediana negativa = golpeas tarde; positiva = golpeas pronto)\n");
	LOG("  ==> offset %d ms\n", offset_ms + p50);
	if (p75 - p25 > 40)
		LOG("  Dispersion alta: el numero es poco fiable, repite mas concentrado.\n");

	// Se guarda ya sumado: las muestras se toman contra el reloj que YA lleva
	// el offset puesto, asi que lo que sale es la correccion sobre el actual.
	cal_mediana = offset_ms + p50;
	cal_disp    = p75 - p25;
}

// Resuelve un golpe del tipo pedido. Devuelve el juicio, o JUICIO_NADA si el
// golpe fue al vacio (que suena pero no cuenta ni como fallo).
// El ultimo parametro es "malos", no "fallos": aqui solo se cuenta el 不可 de
// haber golpeado FUERA de tiempo. Las notas que pasan de largo sin tocarlas
// las cuenta el bucle de juego, en su propio contador.
static int juzgar(int tipo, int ahora, int *perfectos, int *buenos, int *malos)
{
	int idx = nota_mas_cercana(tipo, ahora);
	int dif;

	if (idx < 0) {
		// El golpe al vacio no suena. Antes soltaba el SFX_CANCEL y era
		// justo el ruido que sobraba: en el taiko de verdad, fallar no pita.
		// Tampoco se le pone el sonido normal de golpe, aunque un tambor de
		// verdad suene siempre: aqui SFX_DON es el aviso de "bueno" y
		// SFX_BIGDON el de "perfecto", asi que darselo a un golpe que no ha
		// enganchado nada borraria la unica pista de que SI has acertado.
		return JUICIO_NADA;
	}

	// Con signo y antes del valor absoluto: el signo es justo lo que se
	// busca. Se apunta contra el reloj que YA lleva el offset puesto, para
	// poder recalibrar encima de un valor anterior. Se hace siempre: cuesta
	// un entero por nota y es lo que permite calibrar sin recompilar.
	if (cal_n < MAX_MUESTRAS_CAL)
		cal_muestras[cal_n++] = notas[idx].tiempo_ms - ahora;

	dif = notas[idx].tiempo_ms - ahora;
	if (dif < 0) dif = -dif;
	notas[idx].resuelta = 1;

	if (dif <= ventana_perfecto) {
		(*perfectos)++;
		sfx_pedido[SFX_BIGDON]++;
		return JUICIO_PERFECTO;
	}

	if (dif <= ventana_bueno) {
		(*buenos)++;
		// Una nota grande suena grande aunque el golpe no sea perfecto.
		sfx_pedido[notas[idx].grande ? SFX_BIGDON : SFX_DON]++;
		return JUICIO_BUENO;
	}

	// 不可: la nota se ha enganchado pero tarde (o pronto) de mas. Se come la
	// nota igual y rompe el combo. No suena nada, como cualquier otro fallo.
	(*malos)++;
	return JUICIO_FALLO;
}

// La pausa se dibuja como las demas pantallas y por eso vive con ellas, mas
// abajo; render la llama, asi que hace falta anunciarla aqui.
#define PAUSA_SIGUE       0   // reanudar por donde iba
#define PAUSA_REINICIAR   1   // volver a empezar la misma cancion
#define PAUSA_RESULTADOS  2   // cortar la cancion y ver el resumen
#define PAUSA_MENU        3   // dejarlo y volver al menu, sin resumen

// Lo que render() le dice a main al volver.
#define RENDER_FIN        0   // se acabo (o se dejo): al menu
#define RENDER_REINICIAR  1   // repetir la misma cancion y el mismo curso
static int pantalla_pausa(framebuffer_t *frame, zbuffer_t *z,
                          packet_t *packets[2], const cancion_t *can);

static int render(framebuffer_t *frame, zbuffer_t *z, packet_t *packets[2],
                  const cancion_t *can, int curso)
{
	int context = 0;
	packet_t *current;
	qword_t *dmatag, *q;

	color_t rojo, azul, blanco, amarillo, naranja, gris;

	// Igual que en el menu: la CRUZ con la que se eligio cancion sigue
	// pulsada al entrar aqui, y un SQUARE que viniera de antes contaria
	// como golpe en el primer frame.
	unsigned short prev_btns = 0x0000;
	int ultimo_juicio = JUICIO_NADA;
	int juicio_hasta_ms = 0;
	// Los dos motivos de perder una nota, por separado. Los dos son 不可 en el
	// taiko de verdad, pero mientras practicas no dicen lo mismo: uno es
	// "llegaste tarde" y el otro "ni la viste".
	int perfectos = 0, buenos = 0;
	int malos = 0;      // golpeadas fuera de la ventana buena
	int perdidas = 0;   // pasaron de largo sin tocarlas
	int combo = 0, combo_max = 0;
	int rodillo_golpes = 0;
	int globos_rotos = 0, globos_total = 0;
	int lecturas_pad_malas = 0;
	int salir = 0, fin = 0;
	int cal_guardada = 0;
	int frames_fin = 0;   // fotogramas que lleva puesta la pantalla de resultados
	int puntos = 0;       // shin'uchi: la partitura entera vale un millon
	int puntos_nota = 0;  // lo que vale una nota clavada
	float alma = 0.0f;    // 0..100; al final decide si se aprueba
	float alma_ok = 0.0f, alma_bien = 0.0f, alma_mal = 0.0f;
	int   alma_norma = 60;
#if LOG_FRAMES
	int ahora_prev = -1;
	int dt_max = 0, dt_frames = 0;
	int siguiente_log_frames = 1000;
#endif
	int siguiente_nota = 0;   // primera nota sin pasar, para no recorrer todo
	int fin_chart_ms = 0;
	// Reloj del temblor y suavizado de la energia de la musica.
	float energia_suave = 0.0f;
	float gogo_suave = 0.0f;
	float combo_suave = 0.0f;
	// Cuando reviento un globo, para el destello.
	int globo_roto_hasta = 0;
#if AUTOCICLO
	int frames_auto = 0;
#endif

	// color_t es una UNION, no una struct: con inicializador de llaves
	// gcc solo mete el primer valor y descarta el resto, asi que hay que
	// rellenar campo a campo.
	rojo.r = 0x80; rojo.g = 0x00; rojo.b = 0x00; rojo.a = 0x80; rojo.q = 1.0f;
	azul.r = 0x00; azul.g = 0x00; azul.b = 0x80; azul.a = 0x80; azul.q = 1.0f;
	blanco.r = 0x80; blanco.g = 0x80; blanco.b = 0x80; blanco.a = 0x80; blanco.q = 1.0f;
	amarillo.r = 0x80; amarillo.g = 0x80; amarillo.b = 0x00; amarillo.a = 0x80; amarillo.q = 1.0f;
	naranja.r = 0x80; naranja.g = 0x45; naranja.b = 0x00; naranja.a = 0x80; naranja.q = 1.0f;
	gris.r = 0x30; gris.g = 0x30; gris.b = 0x30; gris.a = 0x80; gris.q = 1.0f;

	// Los globos se cuentan una vez, aqui: en la pantalla de resultados hace
	// falta el total y para entonces ya estan todos marcados como resueltos.
	{
		int k;
		for (k = 0; k < n_notas; k++)
			if (notas[k].tipo == NOTA_GLOBO) globos_total++;
	}

	// El reparto se calcula una vez, aqui: depende de la partitura entera y no
	// cambia durante la partida.
	puntos_nota = calcular_puntos_nota();

	// Y el alma igual.
	{
		int k, juzgables = 0;
		int nivel = (can->nivel[curso] > 0) ? can->nivel[curso] : 1;
		float tasa, dano;

		for (k = 0; k < n_notas; k++)
			if (notas[k].tipo == NOTA_DON || notas[k].tipo == NOTA_KA)
				juzgables++;

		tasa = (nivel <= 7) ? 70.7f : (nivel == 8) ? 70.0f : 75.0f;
		dano = (nivel <= 8) ? 0.625f : 2.0f;

		if (juzgables > 0) {
			alma_ok   = 10000.0f / (juzgables * tasa);
			alma_bien = alma_ok * 0.5f;
			alma_mal  = -alma_ok * dano;
		}
		alma_norma = norma_del_alma(curso, nivel);

		LOG("Alma: %d notas, nivel %d -> +%.4f / +%.4f / %.4f, norma %d%%\n",
		    juzgables, nivel, alma_ok, alma_bien, alma_mal, alma_norma);
	}

	// Donde acaba la chart. Se mira fin_ms y no tiempo_ms: un rodillo de dos
	// segundos acaba mucho despues de donde empieza.
	{
		int k;
		for (k = 0; k < n_notas; k++)
			if (notas[k].fin_ms > fin_chart_ms) fin_chart_ms = notas[k].fin_ms;
	}

	dma_wait_fast();

	for (;;) {
		struct padButtonStatus buttons;
		int ahora, i, don_pulsado, ka_pulsado;
		float t_ruido;
		int dibujadas = 0;
		unsigned short btns;

		current = packets[context];
		ahora = leer_reloj_ms();

		// Tiempo REAL y no tiempo de cancion: el temblor tiene que seguir
		// vivo con el reloj parado (en la pausa, o antes de empezar), o las
		// lineas se congelarian y cantaria.
		t_ruido = reloj_temblor();

		// La energia se suaviza: en crudo pega saltos por trozo y el temblor
		// daria tirones en vez de acompanar.
		energia_suave += ((float)energia / 1000.0f - energia_suave) * 0.12f;

		// Y el Gogo tambien se suaviza, por lo mismo: entrar y salir de golpe
		// del estribillo daria un salto de golpe en las lineas. Con esto la
		// agitacion sube y baja en algo menos de medio segundo.
		{
			float meta = (siguiente_nota < n_notas &&
			              notas[siguiente_nota].gogo) ? 1.0f : 0.0f;
			gogo_suave += (meta - gogo_suave) * 0.06f;
		}

		// El combo, a escalones de diez. Suavizado como los otros dos: el
		// escalon se nota igual, pero entra en un cuarto de segundo en vez de
		// aparecer de golpe en un fotograma.
		{
			int pasos = combo / 10;
			float meta;

			if (pasos > COMBO_TOPE / 10) pasos = COMBO_TOPE / 10;
			meta = (float)pasos / (float)(COMBO_TOPE / 10);
			combo_suave += (meta - combo_suave) * 0.15f;
		}

		amp_temblor = RUIDO_BASE_PX + RUIDO_MUSICA_PX * energia_suave *
		              (1.0f + (GOGO_TEMBLOR  - 1.0f) * gogo_suave) *
		              (1.0f + (COMBO_TEMBLOR - 1.0f) * combo_suave);

		// Ritmo del bucle de dibujo: si un frame tarda mas que la ventana
		// de acierto, hay notas que ningun frame llega a ver a tiempo.
#if LOG_FRAMES
		if (ahora_prev >= 0) {
			int dt = ahora - ahora_prev;
			if (dt > dt_max) dt_max = dt;
			dt_frames++;
			if (ahora >= siguiente_log_frames) {
				LOG("  FRAMES %d dt_max=%d fps=%d\n",
				    ahora, dt_max, dt_frames);
				dt_max = 0; dt_frames = 0;
				siguiente_log_frames += 1000;
			}
		}
		ahora_prev = ahora;
#endif

		// padRead devuelve 0 si no ha podido leer, y en ese caso NO toca
		// la estructura: usarla igual es leer basura de la pila, y salen
		// pulsaciones fantasma en cada frame.
		if (padRead(0, 0, &buttons) != 0) {
			btns = buttons.btns;
		} else {
			btns = 0xFFFF;   // en libpad la logica va invertida: 1 = suelto
			lecturas_pad_malas++;
		}

		// En libpad la logica va invertida (1 = suelto), asi que "recien
		// pulsado" es: suelto antes Y hundido ahora, para alguno de los
		// botones de la mascara. Dos golpes del mismo color en el MISMO
		// frame cuentan como uno; a 50 Hz eso son 20 ms, muy por debajo de
		// cualquier hueco jugable.
		// Durante el margen de entrada NO se deja de juzgar: el reloj va en
		// negativo pero es tiempo de cancion de verdad, asi que un golpe muy
		// adelantado a la primera nota tiene que contar como lo que es.
		don_pulsado = !fin && ((~btns) & prev_btns & BOTONES_DON) != 0;
		ka_pulsado  = !fin && ((~btns) & prev_btns & BOTONES_KA)  != 0;

		// START abre la pausa. Antes cortaba la cancion en seco; eso ahora
		// es una de las dos opciones de dentro. Vale tambien durante el
		// margen de entrada: el hilo de audio ya esta vivo mandando silencio,
		// asi que hay quien conteste.
		if (!fin && !(btns & PAD_START) && (prev_btns & PAD_START)) {
			int r = pantalla_pausa(frame, z, packets, can);

			// Lo que siguiera pulsado al salir de la pausa no cuenta: mismo
			// truco que al entrar en la partida.
			prev_btns = 0x0000;

			// En los dos casos que no son reanudar hay que cortar el audio
			// desde aqui, porque el hilo esta dormido dentro de la pausa y
			// detener_audio es quien lo despierta.
			if (r == PAUSA_REINICIAR) {
				// Igual que PAUSA_MENU en lo que toca al audio (el hilo esta
				// dormido dentro de la pausa y detener_audio es quien lo
				// despierta), pero main vuelve a montar la misma cancion en
				// vez de pasar por el selector.
				detener_audio();
				LOG("Reiniciando la cancion desde la pausa\n");
				return RENDER_REINICIAR;
			}
			if (r == PAUSA_RESULTADOS) {
				// El reloj se queda congelado a proposito: asi el fotograma
				// que falta para llegar a resultados no puede retirar notas.
				detener_audio();
				salir = 1;
			} else if (r == PAUSA_MENU) {
				// Sin resumen: se sale de render y main cierra el ciclo.
				detener_audio();
				LOG("Partida abandonada desde la pausa\n");
				break;
			} else {
				// Reanudando, este fotograma se ha ido en la pausa: se
				// rehace desde arriba, ya con el reloj descongelado.
				continue;
			}
		}

		// En la pantalla de resultados los parches rojos guardan la
		// calibracion medida. Se mira aqui y no arriba porque don_pulsado se
		// anula con fin puesto: en resultados no se juzga nada.
		//
		// Solo desde el metronomo, y solo pasado medio segundo. Las dos cosas
		// son por lo mismo: quien acaba una cancion sigue aporreando un rato,
		// y sin esto un golpe de mas guardaria como calibracion la medida de
		// una cancion cualquiera (que ademas sale sesgada, porque con la
		// ventana estrecha los golpes muy desviados no llegan a contarse).
		if (fin && modo_calibracion && cal_disp >= 0 && !cal_guardada &&
		    frames_fin > 25 && ((~btns) & prev_btns & BOTONES_DON) != 0) {
			offset_ms = cal_mediana;
			cal_guardada = 1;
			guardar_config();
		}
		// De la pantalla de resultados se sale con el parche rojo, que es lo
		// natural con un tambor: es el boton con el que se entra a todo lo
		// demas. START tambien vale, y ese desde el primer fotograma.
		//
		// El rojo NO desde el primer fotograma: quien siga aporreando cuando
		// acaba la cancion se saltaria el resumen sin llegar a verlo. Por eso
		// esto llego a ser solo START; con el segundo de espera se puede tener
		// las dos cosas.
		//
		// Y en el metronomo, mientras quede calibracion por guardar, el rojo
		// es el boton de guardarla (justo abajo): sale a la siguiente.
		if (fin && !(btns & PAD_START) && (prev_btns & PAD_START)) {
			prev_btns = btns;
			break;
		}
		if (fin && frames_fin > 50 &&
		    !(modo_calibracion && cal_disp >= 0 && !cal_guardada) &&
		    ((~btns) & prev_btns & BOTONES_DON) != 0) {
			prev_btns = btns;
			break;
		}

#if AUTOCICLO
		if (!fin && ahora > AUTOCICLO_CORTE_MS) salir = 1;
		if (fin && ++frames_auto > 50) { prev_btns = btns; break; }
#endif

#if AUTOGOLPE
		// La ventana es de 35 ms ([t-10, t+25]) a proposito: el bucle va a
		// 50 Hz (PAL) con picos de 24 ms entre frames, asi que con una
		// ventana mas estrecha que un frame habria notas que ningun frame
		// llega a ver, y saldrian fallos que son de la prueba y no del
		// motor. Con 35 ms siempre cae al menos un frame dentro.
		for (i = siguiente_nota; i < n_notas; i++) {
			if (notas[i].tiempo_ms - ahora > 10) break;
			if (notas[i].resuelta) continue;
			if (ahora - notas[i].tiempo_ms > 25) continue;
			if (notas[i].tipo == NOTA_DON) don_pulsado = 1;
			else                           ka_pulsado  = 1;
		}
		// Y aporrea los rodillos y globos. Sin esto, una vuelta automatica
		// no dice NADA sobre si las ventanas de tramo estan bien: el numero
		// de notas saldria igual con los rodillos rotos.
		if (tramo_activo(siguiente_nota, ahora) >= 0) don_pulsado = 1;
#endif

		// --- Rodillos y globos, ANTES de juzgar notas ---
		//
		// Si el golpe cae dentro de un tramo se lo come el tramo y ya no se
		// juzga contra ninguna nota. Al reves seria peor de lo que parece:
		// un toque dentro del rodillo se comeria la nota que viene detras
		// (nota_mas_cercana busca en +-108 ms), y encima el rodillo no
		// sumaria.
		if (don_pulsado || ka_pulsado) {
			int t = tramo_activo(siguiente_nota, ahora);
			if (t >= 0) {
				notas[t].dados++;
				// Rodillos y globos: 100 por golpe, y el globo NO da premio
				// al reventarlo. En el sistema viejo si (5000), pero en
				// shin'uchi ese premio no existe.
				puntos += PUNTOS_TRAMO;
				if (notas[t].tipo == NOTA_GLOBO) {
					if (notas[t].dados >= notas[t].golpes) {
						notas[t].resuelta = 1;   // reventado
						globos_rotos++;
						sfx_pedido[SFX_BIGDON]++;
						globo_roto_hasta = ahora + 400;
					} else {
						sfx_pedido[SFX_DON]++;
					}
				} else {
					rodillo_golpes++;
					sfx_pedido[notas[t].grande ? SFX_BIGDON : SFX_DON]++;
				}
				don_pulsado = 0;
				ka_pulsado  = 0;
			}
		}

		// --- Juicio por tiempo, no por pixeles ---
		// Don y ka se miran por separado: si caen en el mismo frame, con
		// un solo "tipo" se perderia uno de los dos.
		// Ahora juzgar SI puede devolver fallo: es el 不可, la nota que se
		// engancha fuera de la ventana buena. Se come la nota y rompe el
		// combo, igual que una que se pasa de largo.
		if (don_pulsado) {
			int j = juzgar(NOTA_DON, ahora, &perfectos, &buenos, &malos);
			if (j == JUICIO_PERFECTO || j == JUICIO_BUENO) {
				ultimo_juicio = j; juicio_hasta_ms = ahora + 200;
				if (++combo > combo_max) combo_max = combo;
				puntos += (j == JUICIO_PERFECTO) ? puntos_nota
				                                 : (puntos_nota / 20) * 10;
				alma += (j == JUICIO_PERFECTO) ? alma_ok : alma_bien;
				if (alma > 100.0f) alma = 100.0f;
			} else if (j == JUICIO_FALLO) {
				ultimo_juicio = j; juicio_hasta_ms = ahora + 200;
				combo = 0;
				alma += alma_mal;
				if (alma < 0.0f) alma = 0.0f;
			}
		}
		if (ka_pulsado) {
			int j = juzgar(NOTA_KA, ahora, &perfectos, &buenos, &malos);
			if (j == JUICIO_PERFECTO || j == JUICIO_BUENO) {
				ultimo_juicio = j; juicio_hasta_ms = ahora + 200;
				if (++combo > combo_max) combo_max = combo;
				puntos += (j == JUICIO_PERFECTO) ? puntos_nota
				                                 : (puntos_nota / 20) * 10;
				alma += (j == JUICIO_PERFECTO) ? alma_ok : alma_bien;
				if (alma > 100.0f) alma = 100.0f;
			} else if (j == JUICIO_FALLO) {
				ultimo_juicio = j; juicio_hasta_ms = ahora + 200;
				combo = 0;
				alma += alma_mal;
				if (alma < 0.0f) alma = 0.0f;
			}
		}

		// --- Notas que se han pasado de largo ---
		if (fin) siguiente_nota = n_notas;
		// Ojo: con la ventana de captura ensanchada hay que retirarlas
		// mas tarde tambien, o se marcarian como fallo antes de que la
		// busqueda llegara a engancharlas.
		// Se mira fin_ms y no tiempo_ms: para una nota son lo mismo, pero un
		// rodillo de dos segundos se retiraria (y dejaria de dibujarse) casi
		// dos segundos antes de acabar.
		while (siguiente_nota < n_notas &&
		       notas[siguiente_nota].fin_ms + ventana_activa < ahora) {
			int tp = notas[siguiente_nota].tipo;
			if (!notas[siguiente_nota].resuelta &&
			    (tp == NOTA_DON || tp == NOTA_KA)) {
				// Un rodillo o un globo que se pasa NO es un fallo: en taiko
				// no rompen el combo, simplemente no suman.
				notas[siguiente_nota].resuelta = 1;
				perdidas++;
				alma += alma_mal;
				if (alma < 0.0f) alma = 0.0f;
#if AUTOGOLPE
				LOG("  FALLO nota %d t=%d ahora=%d (retraso %d ms)\n",
				    siguiente_nota, notas[siguiente_nota].tiempo_ms,
				    ahora, ahora - notas[siguiente_nota].tiempo_ms);
#endif
				// Una nota que pasa de largo NO dibuja nada. Las lineas de
				// desanimo son para el mal GOLPE: verlas sin haber tocado
				// dice lo contrario de lo que pasa, y ademas en un tramo
				// perdido saldrian una detras de otra sin parar.
				combo = 0;
			}
			notas[siguiente_nota].resuelta = 1;
			siguiente_nota++;
		}

		// --- Dibujo ---
		dmatag = current->data;
		q = dmatag;
		q++;

		q = draw_disable_tests(q, 0, z);
		q = draw_clear(q, 0, 2048.0f - 320.0f, 2048.0f - 256.0f,
		               frame->width, frame->height, 0x00, 0x00, 0x00);
		q = draw_enable_tests(q, 0, z);

		if (fin) {
			// Pantalla de resultados.
			//
			// Todo lo que se lee vive en la mitad de arriba: la de abajo se
			// deja libre a proposito para el personaje, que tiene que poder
			// seguir ahi al cambiar de pantalla.
			//
			// Las figuras van ANTES que el texto, que estampa la Z maxima y
			// se lleva por delante lo que se mande despues (ver texto()).
			int total = perfectos + buenos + malos + perdidas;
			float bx0 = -SEG_X, bancho = SEG_X * 2.0f;
			float by = 60.0f, balto = 20.0f;
			float x = bx0;
			char linea[80];

			if (total < 1) total = 1;
			frames_fin++;

			// La barra: una tira continua repartida entre los cuatro juicios.
			// En este orden y no en otro, porque asi va de mejor a peor y se
			// lee de un vistazo cuanto hay de cada cosa.
			{
				float w;

				w = bancho * perfectos / total;
				if (w > 0.0f) q = build_rect(q, x, by, w, balto, amarillo);
				x += w;
				w = bancho * buenos / total;
				if (w > 0.0f) q = build_rect(q, x, by, w, balto, blanco);
				x += w;
				w = bancho * malos / total;
				if (w > 0.0f) q = build_rect(q, x, by, w, balto, azul);
				x += w;
				// Las perdidas cierran la tira: lo que queda hasta el final,
				// para que la barra llegue siempre al borde aunque las
				// divisiones dejen algun pixel suelto por el redondeo.
				w = (bx0 + bancho) - x;
				if (w > 0.0f) q = build_rect(q, x, by, w, balto, gris);
			}

			//--- Columna izquierda ---
			{
				char corte[48];

				recorte_sjis(corte, sizeof(corte), can->titulo, 30);
				snprintf(linea, sizeof(linea), "%s - %s", corte,
				         nombre_curso[curso]);
			}
			q = texto(q, -SEG_X, SEG_Y - 16.0f, linea, blanco);

			// La puntuacion y el aprobado se centran SOBRE EL TITULO, no en
			// una x fija. El titulo empieza pegado al margen y su largo
			// cambia con cada cancion, asi que con una x fija los tres
			// quedaban descuadrados en cuanto el nombre no era el de la
			// plantilla. Midiendolo, la columna se centra sola.
			{
				float cx = -SEG_X + ancho_texto(linea) * 0.5f;

				snprintf(linea, sizeof(linea), "%d", puntos);
				q = texto_centrado_en(q, cx, SEG_Y - 54.0f, linea, blanco);

				// Aprobado o no: es lo unico para lo que sirve el alma, y
				// solo se mira aqui. Durante la cancion nunca se muere.
				q = texto_centrado_en(q, cx, SEG_Y - 96.0f,
				                      (alma >= alma_norma) ? "APROBADO"
				                                           : "NO SUPERADO",
				                      (alma >= alma_norma) ? amarillo : gris);
			}

			//--- Columna derecha: los tres juicios ---
			//
			// Cada uno con el color con el que se ve durante la partida: el
			// destello del perfecto es amarillo, el del bien blanco y las
			// lineas del mal azules. Asi la pantalla de resultados usa el
			// mismo idioma que el juego en vez de inventarse otro.
			q = texto_derecha(q, 205.0f, SEG_Y - 16.0f, "Perfecto", amarillo);
			snprintf(linea, sizeof(linea), "%d", perfectos);
			q = texto_derecha(q, SEG_X, SEG_Y - 16.0f, linea, blanco);

			q = texto_derecha(q, 205.0f, SEG_Y - 54.0f, "Bien", blanco);
			snprintf(linea, sizeof(linea), "%d", buenos);
			q = texto_derecha(q, SEG_X, SEG_Y - 54.0f, linea, blanco);

			q = texto_derecha(q, 205.0f, SEG_Y - 92.0f, "Mal", azul);
			snprintf(linea, sizeof(linea), "%d", malos);
			q = texto_derecha(q, SEG_X, SEG_Y - 92.0f, linea, blanco);

			//--- Debajo de la barra ---
			snprintf(linea, sizeof(linea), "Combo max: %d    Rodillos: %d",
			         combo_max, rodillo_golpes);
			q = texto(q, -SEG_X, 34.0f, linea, blanco);

			// La calibracion se queda, pero SOLO en el metronomo y abajo del
			// todo. No es adorno: es donde se guarda, y sin ella el metronomo
			// no sirve para lo que existe.
			if (modo_calibracion) {
				if (cal_disp < 0) {
					snprintf(linea, sizeof(linea),
					         "Calibracion: %d golpes de los 20 que hacen falta",
					         cal_n);
					q = texto(q, -SEG_X, -SEG_Y + 66.0f, linea, gris);
				} else {
					snprintf(linea, sizeof(linea),
					         "Calibracion medida %d ms (ahora %d, dispersion %d)",
					         cal_mediana, offset_ms, cal_disp);
					q = texto(q, -SEG_X, -SEG_Y + 66.0f, linea,
					          (cal_disp > 40) ? gris : blanco);
					if (cal_guardada)
						q = texto(q, -SEG_X, -SEG_Y + 44.0f, config_estado,
						          amarillo);
					else
						q = texto(q, -SEG_X, -SEG_Y + 44.0f,
						          "ROJO guarda esta calibracion en el pen",
						          amarillo);
				}
			}

			q = texto(q, -SEG_X, -SEG_Y, "ROJO vuelve al menu", gris);

			q = draw_finish(q);
			DMATAG_END(dmatag, (q - current->data) - 1, 0, 0, 0);
			dma_wait_fast();
			dma_channel_send_chain(DMA_CHANNEL_GIF, current->data,
			                       q - current->data, 0, 0);
			context ^= 1;
			prev_btns = btns;
			draw_wait_finish();
			graph_wait_vsync();
			continue;
		}

		// El alma: una LINEA en zigzag que crece hacia la derecha en vez de
		// una barra que se rellena. Sale del mismo sitio que el resto del
		// estilo (el obstaculo "onda" de Vib-Ribbon es exactamente esto) y
		// dice mas que un rectangulo: se ve avanzar diente a diente.
		//
		// Va DEBAJO del carril, como en la plantilla, y con las figuras y no
		// con el HUD porque el texto estampa la Z maxima: cualquier figura
		// mandada despues se perderia.
		{
			float x0 = -SANGRE_X, x1 = SANGRE_X;
			float ancho = x1 - x0;
			float fin_x = x0 + ancho * alma / 100.0f;
			float xn = x0 + ancho * alma_norma / 100.0f;
			float ax[160], bx[160];
			int na = 0, nb = 0, k;
			color_t c = (alma >= alma_norma) ? amarillo : naranja;

			// Las x de la primera onda, con el ancho de cada diente sacado
			// del ruido. Se generan hasta pasarse un diente del borde: la
			// punta interpolada necesita SIEMPRE un vertice siguiente al que
			// apuntar, aunque ese ya no se llegue a dibujar.
			{
				float xz = x0;

				for (k = 0; na < 158; k++) {
					ax[na++] = xz;
					if (xz > x1) break;
					xz += ancho_diente(k, 0);
				}
			}

			// Y la segunda, en los puntos medios.
			for (k = 0; k + 1 < na && nb < 158; k++)
				bx[nb++] = (ax[k] + ax[k + 1]) * 0.5f;

			q = onda_alma(q, ax, na, 0,  0, fin_x, c, 400, t_ruido);
			q = onda_alma(q, bx, nb, 1, 23, fin_x, c, 900, t_ruido);

			// La marca de la norma, donde esta el umbral de verdad.
			q = linea(q, xn, ALMA_Y - ALMA_AMP - 5.0f,
			          xn, ALMA_Y + ALMA_AMP + 5.0f, blanco, 460, t_ruido);
		}

		// Las reglas del carril. Sangran hasta el borde a proposito: son
		// lineas horizontales y lo que se pierda por los lados no es
		// informacion. Lo que se LEE se queda dentro de la zona segura.
		q = linea(q, -SANGRE_X, Y_CARRIL + 57.0f, SANGRE_X, Y_CARRIL + 57.0f,
		          gris, 300, t_ruido);
		q = linea(q, -SANGRE_X, Y_CARRIL - 57.0f, SANGRE_X, Y_CARRIL - 57.0f,
		          gris, 320, t_ruido);
		q = linea(q, X_JUEZ + 59.0f, Y_CARRIL + 57.0f,
		          X_JUEZ + 59.0f, Y_CARRIL - 57.0f, gris, 340, t_ruido);

		// El juez: dos aros concentricos y unos radios que los cruzan. Es una
		// figura hecha de lineas, no una forma rellena, que es justo el
		// lenguaje del estilo. Durante el Gogo Time se pone naranja.
		{
			color_t cj = (siguiente_nota < n_notas && notas[siguiente_nota].gogo)
			             ? naranja : blanco;
			int k;

			q = aro(q, X_JUEZ, Y_CARRIL, RADIO_JUEZ,     16, cj, 0,  t_ruido);
			q = aro(q, X_JUEZ, Y_CARRIL, RADIO_JUEZ_INT, 16, cj, 40, t_ruido);
			for (k = 0; k < RADIOS_JUEZ; k++) {
				float a = (float)k * (2.0f * (float)M_PI) / (float)RADIOS_JUEZ;
				float ca = cosf(a), sa = sinf(a);
				q = linea(q,
				          X_JUEZ + RADIO_JUEZ_INT * ca,
				          Y_CARRIL + RADIO_JUEZ_INT * sa,
				          X_JUEZ + RADIO_JUEZ * ca,
				          Y_CARRIL + RADIO_JUEZ * sa,
				          cj, 80 + k * 4, t_ruido);
			}
		}

		// Aviso de golpe: destello de lineas sobre el propio juez, no un
		// circulo de color. Perfecto reparte rayos en TODAS las direcciones,
		// bueno solo en las cuatro cardinales, asi que la diferencia se lee
		// por la forma y no hay que fijarse en el color.
		//
		// El fallo no pinta nada a proposito: un golpe malo no merece
		// celebracion, y el combo cayendo a cero ya lo dice.
		if (ahora < juicio_hasta_ms && ultimo_juicio != JUICIO_NADA) {
			float prog = 1.0f - (float)(juicio_hasta_ms - ahora) / 200.0f;

			if (prog < 0.0f) prog = 0.0f;
			if (ultimo_juicio == JUICIO_PERFECTO)
				q = destello(q, X_JUEZ, Y_CARRIL, 8, prog, amarillo, 360, t_ruido);
			else if (ultimo_juicio == JUICIO_BUENO)
				q = destello(q, X_JUEZ, Y_CARRIL, 4, prog, blanco, 380, t_ruido);
			else
				// El fallo tambien avisa ahora. Antes no pintaba nada, y eso
				// dejaba sin distinguir "le diste mal" de "ni la tocaste",
				// que mientras juegas es informacion util.
				q = desanimo(q, X_JUEZ, Y_CARRIL, prog, azul, 340, t_ruido);
		}

		// Globo recien reventado: trazos rectos hacia fuera.
		if (ahora < globo_roto_hasta) {
			float prog = 1.0f - (float)(globo_roto_hasta - ahora) / 400.0f;
			if (prog < 0.0f) prog = 0.0f;
			q = destello(q, X_JUEZ, Y_CARRIL, 8, prog, naranja, 170, t_ruido);
		}

		// Notas: la posicion sale del tiempo, no de un contador de frames.
		for (i = siguiente_nota; i < n_notas; i++) {
			int dt = notas[i].tiempo_ms - ahora;
			float x = X_JUEZ + dt * PX_POR_MS * notas[i].scroll;
			float r;

			// El corte es por TIEMPO y no por posicion. Antes bastaba con
			// "se ha salido por la derecha, corta": con todas las notas a la
			// misma velocidad, la de detras estaba aun mas lejos. Con #SCROLL
			// eso ya no vale — una nota posterior con scroll bajo puede estar
			// en pantalla cuando la anterior todavia no ha entrado— asi que
			// se corta con la ventana que sale del scroll mas bajo de la
			// chart, y las que caen dentro pero fuera de pantalla se saltan.
			if (dt > ventana_dibujo_ms) break;
			if (notas[i].resuelta) continue;
			if (x > 420.0f) continue;
			// Y por la izquierda tambien. Con un #SCROLL negativo las notas
			// viajan al reves y sin esto ninguna se descartaria nunca: se
			// dibujarian TODAS las de la ventana, que pueden ser cientos,
			// fuera de pantalla.
			//
			// Rodillos y globos NO entran en este corte. Los dos se dibujan
			// en un sitio distinto del que dice su "x": el globo se clava en
			// el juez mientras lo aporreas y el rodillo se recorta solo mas
			// abajo. Cortandolos por x, el globo desaparecia al segundo de
			// abrirse y seguia contando golpes sin verse.
			if (x < -400.0f && notas[i].tipo != NOTA_RODILLO &&
			    notas[i].tipo != NOTA_GLOBO) continue;

			// Tope duro de figuras por fotograma. Jugando no se llega nunca
			// (no caben ni veinte notas en pantalla), pero pasarse del
			// paquete no da un dibujo feo: corrompe memoria y tumba la
			// consola. Con el anillo cada nota cuesta el doble, asi que el
			// margen que habia antes ya no es el mismo.
			if (++dibujadas > 64) break;

			r = notas[i].grande ? RADIO_GRANDE : RADIO_NOTA;

			if (notas[i].tipo == NOTA_RODILLO) {
				// Barra amarilla del principio al final, con las dos puntas
				// redondeadas. Hay que recortarla a mano: un rodillo de 2,5
				// segundos son mas de mil pixeles a PX_POR_MS, y build_rect
				// no recorta nada por su cuenta.
				float x2 = X_JUEZ + (notas[i].fin_ms - ahora) * PX_POR_MS
				           * notas[i].scroll;
				float a = x  < -340.0f ? -340.0f : x;
				float b = x2 >  420.0f ?  420.0f : x2;

				if (estilo_nota == ESTILO_ANILLO) {
					if (b > a) {
						q = linea(q, a, Y_CARRIL + r, b, Y_CARRIL + r,
						          amarillo, 200, t_ruido);
						q = linea(q, a, Y_CARRIL - r, b, Y_CARRIL - r,
						          amarillo, 210, t_ruido);
					}
					if (x  > -340.0f)
						q = aro(q, x,  Y_CARRIL, r, LADOS_NOTA, amarillo,
						        220, t_ruido);
					if (x2 <  420.0f)
						q = aro(q, x2, Y_CARRIL, r, LADOS_NOTA, amarillo,
						        240, t_ruido);
					continue;
				}

				if (b > a)
					q = build_rect(q, a, Y_CARRIL - r, b - a, r * 2.0f,
					               amarillo);
				{
					float dx[64], dy[64];
					int nv;

					if (x > -340.0f) {
						nv = vertices_disco(dx, dy, x, Y_CARRIL, r,
						                    LADOS_NOTA, 220, t_ruido);
						q = relleno(q, dx, dy, nv, x, Y_CARRIL, amarillo);
					}
					if (x2 < 420.0f) {
						nv = vertices_disco(dx, dy, x2, Y_CARRIL, r,
						                    LADOS_NOTA, 240, t_ruido);
						q = relleno(q, dx, dy, nv, x2, Y_CARRIL, amarillo);
					}
				}
				continue;
			}

			if (notas[i].tipo == NOTA_GLOBO) {
				// Mientras esta abierto se queda clavado en el juez, que es
				// donde hay que aporrearlo; antes de llegar viene rodando
				// como una nota mas.
				//
				// Y se HINCHA: el radio va de medio a radio y medio segun los
				// golpes que llevas, asi que se ve crecer de verdad en vez de
				// tener que leer el numero. Debajo cuelga el nudo.
				float xg = (ahora >= notas[i].tiempo_ms) ? X_JUEZ : x;
				float frac = (notas[i].golpes > 0)
				             ? (float)notas[i].dados / (float)notas[i].golpes
				             : 0.0f;
				float rg;
				float nx[3], ny[3];

				if (frac > 1.0f) frac = 1.0f;
				rg = RADIO_GRANDE * (0.55f + 0.75f * frac);

				if (estilo_nota == ESTILO_ANILLO) {
					q = aro(q, xg, Y_CARRIL, rg, LADOS_NOTA, naranja,
					        120, t_ruido);
				} else {
					float dx[64], dy[64];
					int nv = vertices_disco(dx, dy, xg, Y_CARRIL, rg,
					                        LADOS_NOTA, 120, t_ruido);

					q = relleno(q, dx, dy, nv, xg, Y_CARRIL, naranja);
					q = trazo(q, dx, dy, nv, blanco);
				}

				nx[0] = xg - 5.0f;  ny[0] = Y_CARRIL - rg;
				nx[1] = xg;         ny[1] = Y_CARRIL - rg - 9.0f;
				nx[2] = xg + 5.0f;  ny[2] = Y_CARRIL - rg;
				q = poli(q, nx, ny, 3, 0, naranja, 150, t_ruido);
				continue;
			}

			{
				color_t cn = (notas[i].tipo == NOTA_DON) ? rojo : azul;

				if (estilo_nota == ESTILO_ANILLO) {
					// El color vive en el TRAZO y no en el relleno. No se
					// puede quitar aunque el estilo pida blanco: rojo o azul
					// es con que mano golpeas, y en Oni tienes 100 ms para
					// decidirlo. Ahi la legibilidad gana al estilo.
					q = aro(q, x, Y_CARRIL, r, LADOS_NOTA, cn, i * 7, t_ruido);
					q = aro(q, x, Y_CARRIL, r - GROSOR_ANILLO, LADOS_NOTA, cn,
					        i * 7 + 30, t_ruido);
					if (notas[i].grande)
						q = aro(q, x, Y_CARRIL, r + 5.0f, LADOS_NOTA, blanco,
						        i * 7 + 60, t_ruido);
				} else {
					// Relleno macizo pero con la SILUETA temblando y girando:
					// el color se mantiene entero (que es lo que hace que se
					// lea rapido) y lo que se mueve es la forma. El contorno
					// sale de los mismos vertices, asi que por mucho ruido que
					// se meta nunca se despega del relleno.
					float dx[64], dy[64];
					int nv = vertices_disco(dx, dy, x, Y_CARRIL, r,
					                        LADOS_NOTA, i * 7, t_ruido);

					q = relleno(q, dx, dy, nv, x, Y_CARRIL, cn);
					// Las grandes llevan aro blanco, para que se distingan de
					// una normal que venga con otra escala.
					if (notas[i].grande)
						q = trazo(q, dx, dy, nv, blanco);
				}
			}
		}

		// HUD. Va al final de la cadena a proposito: el texto estampa la Z
		// maxima y cualquier figura mandada despues se perderia.
		{
			char linea[64];
			int t;

			// Titulo, puntuacion y combo CENTRADOS, uno sobre otro, como
			// en la plantilla. Antes iban pegados a las esquinas.
			{
				char corte[48];

				recorte_sjis(corte, sizeof(corte), can->titulo, 30);
				snprintf(linea, sizeof(linea), "%s - %s", corte,
				         nombre_curso[curso]);
			}
			q = texto_centrado(q, SEG_Y - 6.0f, linea, blanco);
			snprintf(linea, sizeof(linea), "%d", puntos);
			q = texto_centrado(q, SEG_Y - 42.0f, linea, blanco);
			snprintf(linea, sizeof(linea), "%d COMBO", combo);
			q = texto_centrado(q, SEG_Y - 74.0f, linea, amarillo);

			// Calibrando, cuantos golpes llevan enganchados de los 20 que
			// hacen falta para que la mediana valga algo. Sin esto no hay
			// forma de saber cuando se puede cortar la cancion: el metronomo
			// dura dos minutos y con veinte golpes ya esta medido.
			if (modo_calibracion) {
				int n = (cal_n > 20) ? 20 : cal_n;
				snprintf(linea, sizeof(linea), "Calibracion  %d / 20", n);
				q = texto(q, -SEG_X, SEG_Y - 26.0f, linea,
				          (cal_n >= 20) ? amarillo : gris);
			}

			// Lo que pide el tramo abierto, encima del carril.
			t = tramo_activo(siguiente_nota, ahora);
			if (t >= 0) {
				if (notas[t].tipo == NOTA_GLOBO) {
					int faltan = notas[t].golpes - notas[t].dados;
					snprintf(linea, sizeof(linea), "GLOBO  %d", faltan);
					q = texto(q, X_JUEZ - 30.0f, Y_CARRIL + 70.0f, linea,
					          naranja);
				} else {
					snprintf(linea, sizeof(linea), "%d", notas[t].dados);
					q = texto(q, X_JUEZ - 30.0f, Y_CARRIL + 70.0f, linea,
					          amarillo);
				}
			}
		}

		q = draw_finish(q);

		DMATAG_END(dmatag, (q - current->data) - 1, 0, 0, 0);

		dma_wait_fast();
		dma_channel_send_chain(DMA_CHANNEL_GIF, current->data,
		                       q - current->data, 0, 0);

		context ^= 1;
		prev_btns = btns;

		draw_wait_finish();
		graph_wait_vsync();

		// Fin de partida.
		//
		// Lo normal es esperar a que se acabe el AUDIO: una cancion de verdad
		// suele tener cola despues de la ultima nota y cortarla ahi seria
		// raro. Pero las generadas (metronomo y nivel de prueba) tiran de un
		// .ogg empotrado que dura mucho mas que su chart, y se quedaban
		// sonando minutos sin una sola nota y sin forma de acabar que no
		// fuera la pausa. Para esas se sale en cuanto pasa la ultima nota.
		if (!fin && (salir ||
		             (audio_terminado && siguiente_nota >= n_notas) ||
		             (can->generada && siguiente_nota >= n_notas &&
		              ahora > fin_chart_ms + 2500))) {
			LOG("Resultado: %d perfectos, %d buenos, %d bad, %d perdidas (de %d)\n",
			    perfectos, buenos, malos, perdidas, n_notas);
			LOG("  %d puntos (%d por nota), alma %d%% de %d%% -> %s\n",
			    puntos, puntos_nota, (int)alma, alma_norma,
			    (alma >= alma_norma) ? "APROBADO" : "NO SUPERADO");
			LOG("  combo max %d, rodillos %d golpes, globos %d de %d\n",
			    combo_max, rodillo_golpes, globos_rotos, globos_total);
			LOG("Lecturas de mando fallidas: %d\n", lecturas_pad_malas);
			informe_calibracion();
			// No se sale del bucle: se pasa a la pantalla de resultados y
			// se sigue dibujando. Antes se volvia de render() y main se
			// dormia, con lo que el ultimo fotograma se quedaba clavado en
			// pantalla y parecia que el juego se habia colgado.
			fin = 1;
			salir = 0;

			// El audio se corta AQUI, en seco, y no al salir de la pantalla
			// de resultados.
			//
			// Cuando la cancion acaba sola, el hilo de audio termina pero en
			// la cola del IOP quedan cientos de milisegundos de PCM. Al
			// vaciarse, audsrv no se calla: se queda repitiendo el ultimo
			// trozo, que es esa nota constante que sonaba por debajo de los
			// resultados hasta volver al menu.
			//
			// Cortarlo aqui cuesta una espera de unos milisegundos (lo que
			// tarde el hilo en ver parar_audio, un trozo de ~10 ms) y es
			// ademas lo que va a necesitar el menu de pausa: callar el audio
			// en el mismo frame en que se para el juego.
			detener_audio();

			// Y la marca DESPUES de parar el audio, nunca antes: escribir en
			// el pen pasa por el SIF, y hacerlo con el hilo de audio
			// alimentando a audsrv es pedir un corte de sonido.
			// Se guarda tambien si la cancion se corto con "TERMINAR Y VER
			// RESULTADOS". Antes solo se guardaba la que acababa sola, para
			// que una partida a medias no dejara una marca floja; pero una
			// partida a medias suma menos puntos, asi que no le gana a una
			// entera de todas formas. Lo unico que conseguia era tirar a la
			// basura la puntuacion de quien se sale a falta de diez segundos.
			//
			// "VOLVER AL MENU" sigue sin guardar nada, y eso no cambia: esa
			// opcion no llega a la pantalla de resultados.
			// La marca se guarda igual, aunque la pantalla nueva ya no anuncie
			// que la has batido: eso se quito con el rediseno.
			if (puntos > mejor_puntos &&
			    guardar_puntos(can, curso, puntos) == 0)
				mejor_puntos = puntos;
		}
	}

	return RENDER_FIN;
}

//---------------------------------------------------------------------
// Arranque
//---------------------------------------------------------------------
static void cargar_modulos(void)
{
	int mod_res = -999;

	SifInitRpc(0);
	while (!SifIopReset("", 0)) {}
	while (!SifIopSync()) {}

	SifInitRpc(0);
	SifLoadFileInit();
	SifInitIopHeap();
	sbv_patch_enable_lmb();   // permite SifExecModuleBuffer

	init_scr();
	scr_setXY(0, 1);
	pantalla_texto = 1;
	LOG("Motor de ritmo - prueba de USB\n");

	// USB
	SifExecModuleBuffer(iomanX_irx, size_iomanX_irx, 0, NULL, &mod_res);
	SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, &mod_res);
	fileXioInit();
	SifExecModuleBuffer(bdm_irx, size_bdm_irx, 0, NULL, &mod_res);
	SifExecModuleBuffer(bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL, &mod_res);
	SifExecModuleBuffer(usbd_irx, size_usbd_irx, 0, NULL, &mod_res);
	SifExecModuleBuffer(usbmass_bd_irx, size_usbmass_bd_irx, 0, NULL, &mod_res);

	// Mando. Van de la ROM, pero hay que cargarlos DESPUES del reset del
	// IOP, no antes como hacia Sounds/ (que no reseteaba).
	SifLoadModule("rom0:SIO2MAN", 0, NULL);
	SifLoadModule("rom0:PADMAN", 0, NULL);

	// Audio
	SifLoadModule("rom0:LIBSD", 0, NULL);
	SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &mod_res);

	sleep(1);   // deja que audsrv acabe de registrarse en el IOP
}

// Carga un fichero entero del pen a RAM. "reintentar" espera a que
// aparezca el USB (solo hace falta con el primero que se abre).
static unsigned char *cargar_del_usb(const char *ruta, long *tam_out,
                                     int reintentar)
{
	FILE *f = NULL;
	int intentos = 0;
	long tam;
	size_t leido;
	unsigned char *buf;
	unsigned int t_ini = cpu_ticks();
	int ms_abrir, ms_leer;

	// El pen tarda un poco en aparecer despues de cargar los modulos.
	// Se usa fopen/fread (stdio de newlib con NEWLIB_PORT_AWARE) porque
	// fileXioRead devuelve 0 bytes con buffers de malloc normales.
	do {
		f = fopen(ruta, "rb");
		if (f == NULL && reintentar) {
			intentos++;
			// En una PS2 real el pen tarda entre 1 y 5 segundos en
			// aparecer. Se avisa por pantalla para que un fallo aqui
			// no se confunda con un cuelgue.
			LOG("Esperando al pen... %d/8\n", intentos);
			sleep(1);
		}
	} while (f == NULL && reintentar && intentos < 8);

	if (f == NULL) return NULL;

	// Cuanto cuesta ABRIR y cuanto cuesta LEER, por separado.
	//
	// No es curiosidad: decide como se hace el recorrido de carpetas. Si el
	// coste esta por byte, se puede leer la cabecera de cada .tja en vivo al
	// entrar al menu; si esta por abrir fichero, con 40 canciones el menu
	// tardaria demasiado y habria que guardar un indice en el pen. En PCSX2
	// los dos salen casi cero, asi que este numero solo vale medido en la
	// consola.
	ms_abrir = (int)((long long)(unsigned int)(cpu_ticks() - t_ini)
	                 * 1000 / TICKS_POR_SEG);

	fseek(f, 0, SEEK_END);
	tam = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (tam <= 0) { fclose(f); return NULL; }

	// Alineado a 64: lo que viene del IOP llega por DMA.
	buf = (unsigned char *)memalign(64, (size_t)tam);
	if (buf == NULL) { fclose(f); return NULL; }

	t_ini = cpu_ticks();
	leido = fread(buf, 1, (size_t)tam, f);
	ms_leer = (int)((long long)(unsigned int)(cpu_ticks() - t_ini)
	                * 1000 / TICKS_POR_SEG);
	fclose(f);
	if (leido != (size_t)tam) {
		LOG("Lectura corta: %d de %d\n", (int)leido, (int)tam);
		free(buf);
		return NULL;
	}

	snprintf(ultima_carga, sizeof(ultima_carga),
	         "pen: %dKB  abrir %dms  leer %dms  %dKB/s",
	         (int)(tam / 1024), ms_abrir, ms_leer,
	         ms_leer > 0 ? (int)(tam / ms_leer) : 0);
	LOG("%s: %s\n", ruta, ultima_carga);
	*tam_out = tam;
	return buf;
}

//---------------------------------------------------------------------
// La muestra del selector
//---------------------------------------------------------------------
// Suena mientras se elige dificultad, desde el DEMOSTART que trae el .tja.
//
// La gracia esta en NO leer el .ogg entero: un Ogg Vorbis se puede abrir y
// buscar dentro con solo el principio del fichero, porque la cabecera va
// delante y ov_time_seek() salta por bisección sobre las paginas en vez de
// decodificar todo lo anterior. Lo unico que hace falta es que el trozo
// cargado LLEGUE hasta el instante al que se salta; de ahi la estimacion de
// cuantos bytes pedir.
//
// El trozo no vale para jugar: esta cortado. Por eso vive en su propio buffer
// y no en el cache de cargar_audio_de.
// El buffer NUNCA es suyo: o es el del lector de fondo, o es el .ogg entero
// que ya estaba en RAM de haber jugado la cancion. Por eso aqui no se libera
// nada, solo se para el hilo.
static unsigned char *prev_datos = NULL;
static long           prev_tam   = 0;
static float          prev_desde  = 0.0f;   // segundos del DEMOSTART
static volatile int   prev_parar  = 0;
static volatile int   prev_vivo   = 0;
static int            prev_hilo_id = -1;
static char           prev_ruta[192] = "";  // que .ogg esta cargado ahora

//---------------------------------------------------------------------
// El hilo lector del pen
//---------------------------------------------------------------------
// Lee el .ogg ENTERO a RAM, poco a poco y por detras, mientras el menu sigue
// respondiendo. Dos cosas salen de aqui:
//
//   1. La muestra arranca en cuanto hay bastantes bytes para llegar al
//      DEMOSTART, sin esperar al resto.
//   2. Cuando se elige dificultad NO se tira nada: lo leido ya vale y solo se
//      espera lo que falte. Elegir despues de un rato en la pantalla de
//      dificultad es entrar casi al instante.
//
// Que esto funcione depende de que leer del pen DUERMA al hilo en vez de
// girar. Comprobado con objdump sobre libfileXio: usa WaitSema/SignalSema y
// un semaforo de fin (iSignalSema desde la interrupcion), o sea que suelta la
// CPU mientras el IOP contesta. Es justo lo contrario de graph_wait_vsync,
// que sondea, y por eso aqui si se puede tener un hilo esperando.
#define TROZO_CARGA  (128 * 1024)

static unsigned char  *carga_buf    = NULL;   // del tamaño del fichero entero
static long            carga_total  = 0;
// Cuantos bytes del principio son ya validos. Lo escribe el hilo lector y lo
// lee el menu. No hace falta seqlock como en el reloj: es UNA palabra de 32
// bits alineada, y en el R5900 esa carga o ese guardado no se parten.
static volatile long   carga_validos = 0;
static volatile int    carga_parar  = 0;
static volatile int    carga_viva   = 0;
static int             carga_hilo_id = -1;
static char            carga_ruta[192] = "";

// Bytes que hacen falta para poder saltar al DEMOSTART. Regla de tres entre lo
// que dura la chart y lo que ocupa el fichero, con la mitad de margen porque
// un Vorbis de tasa variable no reparte los bytes por igual: medido con las
// canciones de prueba, sin ese margen el salto de una de las tres se queda
// corto. Si aun asi no llega no pasa nada grave: el salto falla y la muestra
// suena desde el principio.
static long bytes_hasta_demostart(const cancion_t *c, long tam)
{
	int ms_nec = (int)(c->demostart * 1000.0f) + PREVIEW_MS;
	long tope;

	if (c->dur_ms > 1000 && ms_nec > 0)
		// En 64 bits: 10 MB por 80.000 ms se sale de un entero de 32 mucho
		// antes de llegar a la division.
		tope = (long)(((long long)tam * ms_nec * 3) / ((long long)c->dur_ms * 2));
	else
		tope = PREVIEW_MAX_BYTES;

	if (tope < PREVIEW_MIN_BYTES) tope = PREVIEW_MIN_BYTES;
	if (tope > PREVIEW_MAX_BYTES) tope = PREVIEW_MAX_BYTES;
	if (tope > tam)               tope = tam;
	return tope;
}

static int hilo_carga(void *arg)
{
	FILE *f;
	size_t n;

	(void)arg;

	f = fopen(carga_ruta, "rb");
	if (f == NULL) {
		LOG("Carga: no se pudo abrir %s\n", carga_ruta);
		carga_viva = 0;
		ExitDeleteThread();
		return 0;
	}

	// Por trozos y no de una: es lo que permite abortar. Un fread de 10 MB no
	// se puede interrumpir, asi que con trozos de 128 KB lo que se tarda en
	// soltar la carga al volver al menu queda en ~un decimo de segundo.
	while (!carga_parar && carga_validos < carga_total) {
		long queda = carga_total - carga_validos;
		long pide  = (queda > TROZO_CARGA) ? TROZO_CARGA : queda;

		n = fread(carga_buf + carga_validos, 1, (size_t)pide, f);
		if (n == 0) break;
		carga_validos += (long)n;
	}

	fclose(f);
	if (!carga_parar)
		LOG("Carga: %d KB de %s\n", (int)(carga_validos / 1024), carga_ruta);
	carga_viva = 0;
	ExitDeleteThread();
	return 0;
}

static void parar_preview(void);

static void parar_carga(void)
{
	unsigned int t0;

	// SIEMPRE antes de liberar: la muestra puede estar sonando desde este
	// mismo buffer, y soltarlo con el hilo dentro es leer memoria liberada
	// a 48.000 muestras por segundo.
	parar_preview();

	if (carga_hilo_id >= 0) {
		carga_parar = 1;
		// Girar aqui es seguro: el lector tiene mas prioridad que el menu y
		// ademas duerme en cuanto pide datos, asi que avanza igual. Acotado,
		// como todas las esperas del fichero.
		t0 = cpu_ticks();
		while (carga_viva &&
		       (unsigned int)(cpu_ticks() - t0) < (unsigned int)(TICKS_POR_SEG * 3))
			;
		if (carga_viva) LOG("AVISO: el lector del pen no acabo a tiempo\n");
		carga_hilo_id = -1;
	}

	if (carga_buf != NULL) free(carga_buf);
	carga_buf     = NULL;
	carga_total   = 0;
	carga_validos = 0;
	carga_ruta[0] = 0;
}

// Empieza a traerse el .ogg de la cancion. No bloquea mas que lo que cuesta
// abrir el fichero y mirar cuanto ocupa.
static void iniciar_carga(const cancion_t *c)
{
	static unsigned char pila_carga[16 * 1024] __attribute__((aligned(16)));
	ee_thread_t th;
	FILE *f;
	int tid;

	parar_carga();

	// El .ogg de la cancion ANTERIOR se suelta antes de pedir nada: 10 MB de
	// una mas 10 de otra no caben en una consola de 32. Aqui ya se sabe que
	// no es la misma (el menu lo comprueba antes de llamar).
	if (ogg_buffer != NULL) {
		free(ogg_buffer);
		ogg_buffer = NULL;
		ogg_en_cache[0] = 0;
		ogg_cache_tam = 0;
		// Y las que apuntaban ahi tambien: aqui no las lee nadie (en el menu
		// no existe el hilo de la cancion) y cargar_audio_de las vuelve a
		// poner antes de crearlo, pero dejarlas colgando es una mina.
		ogg_datos = NULL;
		ogg_tam   = 0;
	}

	f = fopen(c->ruta_ogg, "rb");
	if (f == NULL) return;
	fseek(f, 0, SEEK_END);
	carga_total = ftell(f);
	fclose(f);
	if (carga_total <= 0) { carga_total = 0; return; }

	// Del tamaño del fichero COMPLETO desde el principio, no del trozo de la
	// muestra: asi al elegir dificultad se sigue rellenando el mismo buffer en
	// vez de tener que pedir otro mas grande y copiar lo ya leido.
	carga_buf = (unsigned char *)memalign(64, (size_t)carga_total);
	if (carga_buf == NULL) {
		LOG("Carga: no caben %d KB para %s\n",
		    (int)(carga_total / 1024), c->ruta_ogg);
		carga_total = 0;
		return;
	}

	snprintf(carga_ruta, sizeof(carga_ruta), "%s", c->ruta_ogg);
	carga_validos = 0;
	carga_parar   = 0;
	carga_viva    = 1;

	th.func             = hilo_carga;
	th.stack            = pila_carga;
	th.stack_size       = sizeof(pila_carga);
	th.gp_reg           = &_gp;
	// Entre el audio (0x40) y el menu (0x50): no puede quitarle el sitio a la
	// muestra que ya este sonando, pero si adelantar al dibujo mientras copia
	// lo que acaba de llegar del IOP.
	th.initial_priority = 0x48;
	th.attr             = 0;
	th.option           = 0;

	ChangeThreadPriority(GetThreadId(), 0x50);

	tid = CreateThread(&th);
	if (tid < 0) {
		LOG("Carga: CreateThread fallo (%d)\n", tid);
		carga_viva = 0;
		free(carga_buf);
		carga_buf = NULL;
		carga_total = 0;
		return;
	}
	carga_hilo_id = tid;
	StartThread(tid, NULL);
}

// Igual que hilo_audio pero sin nada de partida: ni reloj, ni golpes, ni
// pausa. Solo abre el trozo, salta al DEMOSTART y va soltando PCM en bucle.
static int hilo_preview(void *arg)
{
	static fuente_t       fuente_p;
	static OggVorbis_File vf_p;
	// Alineado a 16: el desvanecido lo recorre como muestras de 16 bits, y de
	// paso es lo que quiere el DMA que se lo lleva al IOP.
	static char           pcm_p[TROZO_PCM] __attribute__((aligned(16)));
	vorbis_info *info;
	audsrv_fmt_t fmt;
	int bitstream = 0;
	long leido;
	long long enviados = 0, tope_bytes, fade_bytes;
	int bps;

	(void)arg;

	fuente_p.datos = prev_datos;
	fuente_p.tam   = prev_tam;
	fuente_p.pos   = 0;

	memset(&vf_p, 0, sizeof(vf_p));
	if (ov_open_callbacks(&fuente_p, &vf_p, NULL, 0, cbs_mem) != 0) {
		LOG("Muestra: no se pudo abrir el trozo\n");
		prev_vivo = 0;
		ExitDeleteThread();
		return 0;
	}

	info = ov_info(&vf_p, -1);
	bps = info->rate * info->channels * 2;
	fmt.freq     = info->rate;
	fmt.bits     = 16;
	fmt.channels = info->channels;
	if (audsrv_set_format(&fmt) != 0)
		LOG("Muestra: audsrv_set_format: %s\n", audsrv_get_error_string());
	audsrv_set_volume(MAX_VOLUME * vol_musica / VOL_PASOS);

	// Si el trozo no llega hasta el DEMOSTART (la estimacion se quedo corta,
	// o la chart acaba mucho antes que el audio) se oye desde el principio,
	// que es peor muestra pero mejor que el silencio.
	if (prev_desde > 0.0f && ov_time_seek(&vf_p, (double)prev_desde) != 0) {
		LOG("Muestra: no se llego a %d s, se oye desde el principio\n",
		    (int)prev_desde);
		prev_desde = 0.0f;
		ov_raw_seek(&vf_p, 0);
	}

	tope_bytes = (long long)bps * PREVIEW_MS / 1000;
	fade_bytes = (long long)bps * PREVIEW_FADE_MS / 1000;
	// Por si algun dia el desvanecido se pone mas largo que la propia muestra:
	// sin esto el factor saldria negativo y el audio, basura.
	if (fade_bytes > tope_bytes) fade_bytes = tope_bytes;

	while (!prev_parar) {
		leido = ov_read(&vf_p, pcm_p, TROZO_PCM, 0, 2, 1, &bitstream);

		// Fin del trozo cargado, o ya sonaron los segundos que toca. Suena UNA
		// vez y se calla: en bucle se hace pesadisimo mientras eliges.
		if (leido <= 0 || enviados >= tope_bytes) break;

		// Desvanecido del final, muestra a muestra. El factor va en 0..256 y
		// se aplica con un desplazamiento: multiplicar por los bytes que
		// quedan directamente se saldria de un entero de 32 bits (32767 por
		// 192000 ya no cabe).
		if (enviados + leido > tope_bytes - fade_bytes) {
			short *m = (short *)pcm_p;
			int n = (int)leido / 2;   // muestras de 16 bits con signo
			int i;

			for (i = 0; i < n; i++) {
				long long queda = tope_bytes - (enviados + (long long)i * 2);
				int f;

				if (queda <= 0)               f = 0;
				else if (queda >= fade_bytes) f = 256;
				else                          f = (int)(queda * 256 / fade_bytes);

				m[i] = (short)(((int)m[i] * f) >> 8);
			}
		}

		audsrv_wait_audio((int)leido);
		if (prev_parar) break;
		audsrv_play_audio(pcm_p, (int)leido);
		enviados += leido;
	}

	// Y apagar bien, que no es solo dejar de mandar: al vaciarse sola, audsrv
	// no se calla, se queda repitiendo el ultimo trozo (el mismo zumbido que
	// salia al acabar una cancion). Pero parar en seco aqui se comeria la cola
	// que aun no ha sonado, o sea el final de la muestra.
	//
	// Asi que se manda silencio hasta cubrir de sobra lo que quepa en la cola:
	// para cuando termina, la muestra ya se ha oido entera y lo unico que
	// queda por sonar son ceros. Entonces se para.
	//
	// Con audsrv_wait_audio y no con una espera en vacio, por lo de siempre:
	// este hilo tiene mas prioridad que el del menu y girar aqui lo dejaria
	// sin CPU.
	if (!prev_parar) {
		long long cola = (long long)bps * 400 / 1000;
		long long puesto = 0;

		memset(pcm_p, 0, sizeof(pcm_p));
		while (!prev_parar && puesto < cola) {
			audsrv_wait_audio(TROZO_PCM);
			if (prev_parar) break;
			audsrv_play_audio(pcm_p, TROZO_PCM);
			puesto += TROZO_PCM;
		}
		audsrv_stop_audio();
	}

	ov_clear(&vf_p);
	prev_vivo = 0;
	ExitDeleteThread();
	return 0;
}

static void parar_preview(void)
{
	unsigned int t0;

	if (prev_hilo_id < 0) return;

	prev_parar = 1;
	// Aqui se puede girar en vacio: el hilo de la muestra tiene mas
	// prioridad que este, asi que lo desaloja y avanza. Se acota igual,
	// que es lo que hace el resto del fichero.
	t0 = cpu_ticks();
	while (prev_vivo &&
	       (unsigned int)(cpu_ticks() - t0) < (unsigned int)TICKS_POR_SEG)
		;
	if (prev_vivo) LOG("AVISO: la muestra no solto audsrv a tiempo\n");
	prev_hilo_id = -1;
	// Lo que quedara en la cola del IOP, fuera: si no, se oye un cacho de
	// la muestra por debajo de la cancion al empezar a jugar.
	audsrv_stop_audio();

	prev_datos = NULL;
	prev_tam   = 0;
	prev_ruta[0] = 0;
}

// Se llama en CADA fotograma de la pantalla de dificultad. No hace nada hasta
// que el lector de fondo ha traido bastante para saltar al DEMOSTART; entonces
// arranca la muestra y ya no vuelve a entrar.
static void intentar_preview(const cancion_t *c)
{
	static unsigned char pila_prev[32 * 1024] __attribute__((aligned(16)));
	ee_thread_t th;
	int tid;

	if (prev_hilo_id >= 0) return;                 // ya esta sonando
	// Las generadas (metronomo, nivel de prueba) no tienen .ogg que enseñar.
	if (c == NULL || c->generada || c->ruta_ogg[0] == 0) return;

	if (ogg_buffer != NULL && strcmp(ogg_en_cache, c->ruta_ogg) == 0) {
		// La cancion entera ya esta en RAM de haberla jugado: ni lector ni
		// espera, y ademas el salto no se puede quedar corto.
		prev_datos = ogg_buffer;
		prev_tam   = ogg_cache_tam;
	} else {
		if (carga_buf == NULL || strcmp(carga_ruta, c->ruta_ogg) != 0) return;
		if (carga_validos < bytes_hasta_demostart(c, carga_total)) return;

		prev_datos = carga_buf;
		// Una FOTO de lo que hay leido ahora, no el tamaño final: el lector
		// sigue rellenando por detras y la muestra no debe mirar ahi (serian
		// bytes sin escribir todavia). Con esto libvorbisfile trata el trozo
		// como si el fichero acabara ahi, que para ocho segundos sobra.
		prev_tam = carga_validos;
	}

	snprintf(prev_ruta, sizeof(prev_ruta), "%s", c->ruta_ogg);
	prev_desde = (c->demostart > 0.0f) ? c->demostart : 0.0f;
	prev_parar = 0;
	prev_vivo  = 1;

	th.func             = hilo_preview;
	th.stack            = pila_prev;
	th.stack_size       = sizeof(pila_prev);
	th.gp_reg           = &_gp;
	// Misma prioridad que el hilo de la cancion, y por lo mismo: el bucle del
	// menu sondea el vsync sin dormirse, asi que con menos prioridad esta se
	// quedaria sin CPU y se oiria a trompicones.
	th.initial_priority = 0x40;
	th.attr             = 0;
	th.option           = 0;

	ChangeThreadPriority(GetThreadId(), 0x50);

	tid = CreateThread(&th);
	if (tid < 0) {
		LOG("Muestra: CreateThread fallo (%d)\n", tid);
		// Sin liberar nada: el buffer es del lector (o del cache del .ogg),
		// aqui solo se suelta la referencia.
		prev_vivo  = 0;
		prev_datos = NULL;
		prev_tam   = 0;
		return;
	}
	prev_hilo_id = tid;
	StartThread(tid, NULL);
}

// Nota: aqui vivia cargar_del_usb_variantes(), que probaba tambien el nombre
// en mayusculas por si el fichero habia quedado guardado en 8.3. Ya no hace
// falta y ademas estorbaba: ahora los nombres salen del listado real del
// directorio, asi que vienen con la caja exacta que tienen en el FAT. Y con
// carpetas de por medio, poner en mayusculas "a partir del septimo caracter"
// habria destrozado el nombre de la carpeta.

static void init_mando(void)
{
	static char padBuf[256] __attribute__((aligned(64)));
	int state;

	padInit(0);
	padPortOpen(0, 0, padBuf);

	state = padGetState(0, 0);
	while (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)
		state = padGetState(0, 0);
}

static void lanzar_hilo_audio(void)
{
	static unsigned char pila[64 * 1024] __attribute__((aligned(16)));
	ee_thread_t th;
	int tid;

	th.func             = hilo_audio;
	th.stack            = pila;
	th.stack_size       = sizeof(pila);
	th.gp_reg           = &_gp;
	// Numero mas bajo = mas prioridad. El audio va por delante del dibujo
	// para que no se corte nunca; se bloquea solo en audsrv_wait_audio,
	// asi que deja correr al bucle de juego el resto del tiempo.
	th.initial_priority = 0x40;
	th.attr             = 0;
	th.option           = 0;

	// El hilo principal se baja de prioridad a proposito: no se puede dar
	// por hecho con cual arranca, y si quedara por encima del de audio,
	// graph_wait_vsync lo dejaria sin CPU y la musica se cortaria.
	ChangeThreadPriority(GetThreadId(), 0x50);

	tid = CreateThread(&th);
	if (tid < 0) {
		LOG("CreateThread fallo: %d\n", tid);
		return;
	}
	// Se guarda porque la pausa lo duerme con SleepThread y hace falta el id
	// para despertarlo.
	hilo_audio_id = tid;
	StartThread(tid, NULL);
}


//---------------------------------------------------------------------
// Catalogo, carga y ciclo de vida de una cancion
//---------------------------------------------------------------------

// Extension de un nombre, sin distinguir mayusculas. En FAT los nombres
// pueden llegar en 8.3 en mayusculas o con nombre largo tal cual se copiaron,
// y no se puede dar por hecho ninguna de las dos formas.
static int termina_en(const char *nombre, const char *ext)
{
	size_t ln = strlen(nombre), le = strlen(ext);
	size_t i;

	if (ln < le) return 0;
	for (i = 0; i < le; i++) {
		char a = nombre[ln - le + i], b = ext[i];
		if (a >= 'A' && a <= 'Z') a += 32;
		if (b >= 'A' && b <= 'Z') b += 32;
		if (a != b) return 0;
	}
	return 1;
}

static int igual_sin_mayus_s(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return 0;
	}
	return *a == *b;
}

// El bit de directorio llega en un sitio o en otro segun el driver: iomanX
// usa FIO_S_IFDIR (0x1000) y ioman el FIO_SO_IFDIR (0x0020). bdmfs_fatfs va
// por iomanX, pero se miran los dos. Equivocarse aqui es no ver ninguna
// carpeta y no tener ni idea de por que.
static int es_carpeta(const iox_stat_t *st)
{
	return FIO_S_ISDIR(st->mode) || FIO_SO_ISDIR(st->mode);
}

// Las dos canciones que no salen del pen, con la chart hecha por codigo:
//
//   Metronomo 140  una nota por pulso. Sirve para que el juego arranque y sea
//                  jugable sin pen, que es como se prueba en PCSX2 la mitad
//                  de las veces, y para comprobar que el reloj cuadra con los
//                  clics del audio.
//   Prueba de notas  un grupo por cada tipo que sabe dibujar el motor.
//
// Las dos tiran del click140.ogg empotrado. Van siempre al final de la lista.
static void anadir_canciones_generadas(void)
{
	// El metronomo es ademas la pista de calibracion: un clic por pulso y
	// nada mas, que es justo lo que hace falta para medir el desfase.
	//
	// El nombre de la pista de prueba lleva kana y kanji A PROPOSITO: es la
	// unica forma de comprobar en la consola que el texto japones se pinta
	// bien sin depender de que haya un .tja japones en el pen. Las tres
	// canciones de prueba traen el TITLE en latino y el japones en TITLEJA,
	// asi que con ellas no se ejercita este camino.
	static const char *nombres[2] = {
		"Metronomo (calibrar)",
		"Prueba de notas (かな漢字)"
	};
	static const int   tipos[2]   = { GEN_METRONOMO, GEN_PRUEBA };
	int j, k;

	// Todo lo catalogado hasta aqui viene del pen: eso es lo que se enseña.
	n_visibles = n_canciones;

	for (j = 0; j < 2 && n_canciones < MAX_CANCIONES; j++) {
		cancion_t *c = &canciones[n_canciones++];

		memset(c, 0, sizeof(*c));
		// Por el mismo camino que los titulos del pen: si algun dia un nombre
		// generado lleva algo que no sea ASCII, ya esta convertido.
		a_sjis(c->titulo, sizeof(c->titulo), nombres[j]);
		c->generada = tipos[j];
		for (k = 0; k < N_CURSOS; k++) c->nivel[k] = -1;
		c->nivel[0] = 1;   // solo tienen un "curso"
	}
}

// Lee un .tja ya localizado y lo mete en el catalogo. "ruta_ogg" es la que
// se haya resuelto mirando la carpeta; puede venir vacia si no habia audio.
//
// Solo se guarda la cabecera (titulo y nivel de cada curso). Las notas se
// vuelven a parsear al elegir la cancion, porque una chart entera son ~50 KB
// y no caben 32 en RAM.
static void anadir_cancion(const char *ruta_tja, const char *ruta_ogg)
{
	static tja_chart_t tmp;   // ~50 KB: static, no cabe en la pila
	unsigned char *datos;
	long tam = 0;
	cancion_t *c;
	int k, cursos = 0;

	if (n_canciones >= MAX_CANCIONES - 2) return;   // los huecos de las generadas

	datos = cargar_del_usb(ruta_tja, &tam, 0);
	if (datos == NULL) {
		LOG("  no se pudo leer %s\n", ruta_tja);
		return;
	}

	c = &canciones[n_canciones];
	memset(c, 0, sizeof(*c));
	snprintf(c->ruta_tja, sizeof(c->ruta_tja), "%s", ruta_tja);
	snprintf(c->ruta_ogg, sizeof(c->ruta_ogg), "%s", ruta_ogg ? ruta_ogg : "");

	for (k = 0; k < N_CURSOS; k++) {
		c->nivel[k] = -1;
		if (tja_parsear((const char *)datos, tam, clave_curso[k], &tmp) != 0)
			continue;
		// El LEVEL puede venir con decimales (splice trae "11.7"), asi que
		// atoi y a correr: el numero solo se enseña.
		c->nivel[k] = atoi(tmp.nivel);
		cursos++;

		// De la cabecera, iguales en todos los cursos: se cogen del primero
		// que parsee. La duracion SI cambia de un curso a otro, y lo que
		// interesa es la mas larga (es la que mejor se acerca a lo que dura
		// el audio, que es lo que se quiere estimar).
		if (c->demostart <= 0.0f) c->demostart = tmp.demostart;
		if (tmp.dur_ms > c->dur_ms) c->dur_ms = tmp.dur_ms;
		// El titulo es de la cancion, no del curso: se coge del primero que
		// parsee bien.
		//
		// Y se pasa a Shift-JIS AQUI, una sola vez. A partir de este punto
		// todo el motor trabaja con el titulo ya convertido: pintarlo,
		// medirlo y recortarlo. Convertirlo en cada fotograma seria absurdo,
		// y hacerlo mas tarde obligaria a acordarse en cada sitio que lo
		// pinta, que son seis.
		if (c->titulo[0] == 0) {
			char conv[96];

			a_sjis(conv, sizeof(conv),
			       tmp.titulo[0] ? tmp.titulo : "sin titulo");
			recorte_sjis(c->titulo, sizeof(c->titulo), conv,
			             (int)sizeof(c->titulo) - 1);
		}
	}
	free(datos);

	if (cursos == 0) {
		LOG("  %s no trae ningun curso legible\n", ruta_tja);
		return;
	}

	{
		// Se enseña el .ogg que se ha emparejado, no la clave WAVE del .tja:
		// si el FAT ha devuelto nombres 8.3 no coincidiran, y saber cual se
		// va a abrir de verdad es lo primero que hace falta si no suena.
		const char *nom = strrchr(c->ruta_ogg, '/');
		LOG("  %-22s %d cursos  %s\n", c->titulo, cursos,
		    nom ? nom + 1 : "(sin audio)");
	}
	n_canciones++;
}

// Recorre una carpeta: si tiene un .tja lo cataloga, y en cualquier caso baja
// a las subcarpetas mientras quede profundidad. Asi valen las tres formas que
// se ven en la practica: .tja suelto en la raiz, una carpeta por cancion, y
// una capa de genero por encima de las carpetas de cancion.
static void escanear_carpeta(const char *ruta, int prof)
{
	// Los nombres se apuntan ANTES de tocar nada mas. No se puede tener dos
	// directorios abiertos y bajar recursivamente con el de arriba a medias:
	// hay drivers que no lo llevan bien, y depurar eso en una consola sin
	// consola no es plan.
	char tja[256];
	char oggs[MAX_OGG_CARPETA][256];
	char subs[MAX_CANCIONES][256];
	int n_ogg = 0, n_sub = 0;
	int fd, i;
	iox_dirent_t ent;
	char sub[192];

	if (prof > PROF_MAX) return;

	fd = fileXioDopen(ruta);
	if (fd < 0) return;

	tja[0] = 0;
	while (fileXioDread(fd, &ent) > 0) {
		if (ent.name[0] == '.') continue;          // "." ".." y ocultos

		if (es_carpeta(&ent.stat)) {
			if (n_sub < MAX_CANCIONES)
				snprintf(subs[n_sub++], sizeof(subs[0]), "%s", ent.name);
		} else if (termina_en(ent.name, ".tja")) {
			if (tja[0] == 0)
				snprintf(tja, sizeof(tja), "%s", ent.name);
		} else if (termina_en(ent.name, ".ogg")) {
			if (n_ogg < MAX_OGG_CARPETA)
				snprintf(oggs[n_ogg++], sizeof(oggs[0]), "%s", ent.name);
		}
	}
	fileXioDclose(fd);

	if (tja[0]) {
		char ruta_tja[192], ruta_ogg[192];
		int elegido = -1;

		// Con precision explicita en los dos trozos: un nombre del FAT puede
		// venir de hasta 255 caracteres y el destino son 192. Truncar una
		// ruta la deja sin abrir, pero desbordar seria mucho peor.
		snprintf(ruta_tja, sizeof(ruta_tja), "%.110s/%.72s", ruta, tja);

		// Cual de los .ogg de la carpeta es el bueno. Lo dice la clave WAVE
		// del .tja, pero no se puede fiar del nombre tal cual: si el fichero
		// quedo guardado en 8.3 (AFTER_~1.OGG) no coincidira con
		// "after_epochs.ogg". Se compara sin mayusculas y, si no cuadra
		// ninguno, se coge el primero que haya: una carpeta de cancion no
		// suele tener dos.
		if (n_ogg > 0) {
			static tja_chart_t cab;
			unsigned char *d;
			long t = 0;

			d = cargar_del_usb(ruta_tja, &t, 0);
			if (d != NULL) {
				int k;
				for (k = 0; k < N_CURSOS; k++)
					if (tja_parsear((const char *)d, t, clave_curso[k], &cab) == 0)
						break;
				if (k < N_CURSOS && cab.wave[0]) {
					for (i = 0; i < n_ogg; i++)
						if (igual_sin_mayus_s(oggs[i], cab.wave)) { elegido = i; break; }
				}
				free(d);
			}
			if (elegido < 0) elegido = 0;
			snprintf(ruta_ogg, sizeof(ruta_ogg), "%.110s/%.72s",
			         ruta, oggs[elegido]);
		} else {
			ruta_ogg[0] = 0;
			LOG("  %s: sin .ogg en la carpeta\n", ruta);
		}

		anadir_cancion(ruta_tja, ruta_ogg);
		// Y se SIGUE bajando. Cortar aqui parecia razonable ("una cancion
		// por carpeta") pero rompia el caso mas normal de todos: un .tja
		// suelto en la raiz del pen hacia que la raiz contara como cancion
		// y no se mirara ni una carpeta.
	}

	// Las subcarpetas: canciones, o una capa de genero por encima.
	for (i = 0; i < n_sub; i++) {
		snprintf(sub, sizeof(sub), "%.110s/%.72s", ruta, subs[i]);
		escanear_carpeta(sub, prof + 1);
	}
}

// Ordena por titulo, sin distinguir mayusculas. Insercion directa: con 32
// como mucho, cualquier cosa mas lista seria mas codigo para nada.
static void ordenar_canciones(int n)
{
	static cancion_t aux;
	int i, j;

	for (i = 1; i < n; i++) {
		aux = canciones[i];
		for (j = i - 1; j >= 0; j--) {
			const char *a = canciones[j].titulo, *b = aux.titulo;
			int mayor = 0;
			while (*a && *b) {
				char x = *a, y = *b;
				if (x >= 'A' && x <= 'Z') x += 32;
				if (y >= 'A' && y <= 'Z') y += 32;
				if (x != y) { mayor = (x > y); break; }
				a++; b++;
			}
			if (!*a && !*b) mayor = 0;
			else if (!*a) mayor = 0;
			else if (!*b) mayor = 1;
			if (!mayor) break;
			canciones[j + 1] = canciones[j];
		}
		canciones[j + 1] = aux;
	}
}

// Espera a que el pen aparezca y recorre lo que haya.
//
// Antes el que esperaba era el fopen del .tja fijo; ahora lo primero que se
// toca es el directorio raiz, asi que la espera vive aqui. En una PS2 real
// el pen tarda entre 1 y 5 segundos en montarse.
static void catalogar_canciones(void)
{
	int fd = -1, intentos = 0;

	n_canciones = 0;

	while (intentos < 8) {
		fd = fileXioDopen(RAIZ_PEN);
		if (fd >= 0) break;
		intentos++;
		LOG("Esperando al pen... %d/8\n", intentos);
		sleep(1);
	}
	if (fd < 0) {
		LOG("No hay pen en %s: solo estara el metronomo\n", RAIZ_PEN);
		anadir_canciones_generadas();
		return;
	}
	fileXioDclose(fd);

	LOG("Recorriendo %s\n", RAIZ_PEN);
	escanear_carpeta("mass0:", 0);

	if (n_canciones == 0)
		LOG("No se encontro ninguna cancion\n");
	else
		LOG("%d canciones\n", n_canciones);

	ordenar_canciones(n_canciones);

	// Las generadas se quedan siempre al final, y por eso se añaden DESPUES
	// de ordenar.
	anadir_canciones_generadas();
}

// Primer curso que existe a partir de "desde", andando en la direccion que
// diga "paso". Devuelve "desde" si no hay ninguno mas.
static int curso_valido(const cancion_t *c, int desde, int paso)
{
	int k = desde;
	int vueltas;

	for (vueltas = 0; vueltas < N_CURSOS; vueltas++) {
		k += paso;
		if (k < 0 || k >= N_CURSOS) break;
		if (c->nivel[k] >= 0) return k;
	}
	// Nunca negativo: el que llama lo usa como indice de nivel[] y de
	// nombre_curso[] sin mirar. Se llama con desde = -1 para "el primero
	// que haya", y si esa busqueda no encontrara nada devolveria -1.
	return (desde >= 0) ? desde : 0;
}

// Deja el .ogg de la cancion en ogg_datos/ogg_tam. Devuelve 0 si es el que
// pedia la partitura y -1 si ha habido que tirar del empotrado.
static int cargar_audio_de(const cancion_t *c)
{
	char ruta[192];
	long tam = 0;
	unsigned char *buf;

	if (c->generada) {
		ogg_datos = click140_ogg;
		ogg_tam   = (long)size_click140_ogg;
		return 0;
	}

	if (c->ruta_ogg[0] == 0) {
		LOG("'%s' no tiene .ogg: se usa el empotrado\n", c->titulo);
		ogg_datos = click140_ogg;
		ogg_tam   = (long)size_click140_ogg;
		return -1;
	}
	snprintf(ruta, sizeof(ruta), "%s", c->ruta_ogg);

	// Volver a leer 4 MB del pen son varios segundos en la consola (el bus
	// USB 1.1 da ~1 MB/s), asi que repetir cancion no vuelve a leer nada.
	if (ogg_buffer != NULL && strcmp(ogg_en_cache, ruta) == 0) {
		LOG("%s ya estaba en RAM (%d bytes)\n", ruta, (int)ogg_cache_tam);
		ogg_datos = ogg_buffer;
		ogg_tam   = ogg_cache_tam;
		return 0;
	}

	if (ogg_buffer != NULL) {
		free(ogg_buffer);
		ogg_buffer = NULL;
		ogg_en_cache[0] = 0;
		ogg_cache_tam = 0;
	}

	buf = cargar_del_usb(ruta, &tam, 0);
	if (buf == NULL) {
		LOG("Sin %s: tirando del .ogg empotrado\n", ruta);
		ogg_datos = click140_ogg;
		ogg_tam   = (long)size_click140_ogg;
		return -1;
	}

	ogg_buffer = buf;
	ogg_cache_tam = tam;
	snprintf(ogg_en_cache, sizeof(ogg_en_cache), "%s", ruta);
	ogg_datos = buf;
	ogg_tam   = tam;
	return 0;
}

// Todo lo que hay que dejar como estaba antes de arrancar el hilo de audio.
//
// Cada linea de aqui corresponde a una forma distinta de que la SEGUNDA
// cancion salga mal, y ninguna se parece a un fallo de audio: por eso van
// juntas en un sitio en vez de repartidas por main.
static void preparar_cancion(void)
{
	int i;

	// Las banderas ANTES de CreateThread, nunca despues: el hilo arranca de
	// inmediato y se compite con el.
	audio_listo      = 0;
	audio_terminado  = 0;   // si se queda a 1, render se va derecho a resultados
	parar_audio      = 0;
	audio_parado     = 0;
	// Las de la pausa tambien, y por el mismo motivo que las de arriba: si se
	// volvio al menu DESDE la pausa, pausa_pedida se quedaria a 1 y el hilo de
	// la cancion siguiente se dormiria en su primera vuelta. Sin musica y sin
	// reloj, eso se ve como un cuelgue.
	pausa_pedida     = 0;
	pausa_activa     = 0;
	hilo_audio_id    = -1;
	reloj_congelado  = 0;
	reloj_arrancado  = 0;
	duracion_ms      = 0;   // main comprueba "terminado && duracion == 0"
	reloj_estable    = 0;
	error_interp_max = 0;
	reloj_version    = 0;
	reloj_ms_base    = 0;
	reloj_ticks_base = 0;

	// Si el ultimo frame de la partida anterior pidio un golpe cuando el
	// hilo ya habia salido, el hilo nuevo lo serviria nada mas nacer y
	// sonaria un golpe fantasma en el primer frame.
	for (i = 0; i < N_SFX; i++) sfx_servido[i] = sfx_pedido[i];

	// Las muestras de calibracion son de UNA partida: mezclarlas con las de
	// la anterior daria una mediana de dos canciones distintas.
	cal_n = 0;

	// Y las notas. chart_desde_tja ya las deja a cero, pero si algun dia se
	// reutiliza la chart sin volver a parsear, sin esto la segunda vuelta
	// empezaria con todo marcado como resuelto: ni una nota en pantalla y
	// la cancion "terminada" al instante.
	for (i = 0; i < n_notas; i++) notas[i].resuelta = 0;
}

// Corta la cancion y espera a que el hilo de audio suelte audsrv.
// Idempotente a proposito: la llama render() en cuanto acaba la cancion y la
// vuelve a llamar main() al cerrar el ciclo. La segunda vez no hace nada.
static void detener_audio(void)
{
	unsigned int t0;

	if (audio_parado) return;
	audio_parado = 1;

	parar_audio = 1;

	// Si estaba en pausa hay que despertarlo, o se quedaria dormido para
	// siempre: la espera de abajo se comeria sus tres segundos, saldria el
	// aviso y el hilo se quedaria vivo con audsrv cogido.
	pausa_pedida = 0;
	if (hilo_audio_id >= 0 && !audio_terminado) WakeupThread(hilo_audio_id);

	// Aqui main SI puede girar en vacio: el hilo de audio tiene mas
	// prioridad, asi que lo desaloja y avanza. Aun asi se acota, para no
	// colgarse si algo fue mal.
	t0 = cpu_ticks();
	while (!audio_terminado &&
	       (unsigned int)(cpu_ticks() - t0) < (unsigned int)(TICKS_POR_SEG * 3))
		;
	if (!audio_terminado)
		LOG("AVISO: el hilo de audio no acabo a tiempo\n");

	// Lo que quedaba en la cola lo sigue tocando el IOP el solo. Durante la
	// cancion eso esta bien y es a proposito (ver hilo_audio), pero al
	// volver al menu se oiria la cola de la cancion anterior por debajo. Se
	// llama desde aqui y no desde el hilo porque el hilo ya no existe.
	audsrv_stop_audio();

	pausa_activa  = 0;
	hilo_audio_id = -1;
}


//---------------------------------------------------------------------
// Pantallas
//---------------------------------------------------------------------

// Empieza una cadena DMA nueva con la pantalla ya limpia. Las tres
// pantallas (menu, cargando, partida) hacen exactamente lo mismo aqui.
static qword_t *abrir_frame(qword_t *dmatag, framebuffer_t *frame, zbuffer_t *z)
{
	qword_t *q = dmatag + 1;

	q = draw_disable_tests(q, 0, z);
	q = draw_clear(q, 0, 2048.0f - 320.0f, 2048.0f - 256.0f,
	               frame->width, frame->height, 0x00, 0x00, 0x00);
	q = draw_enable_tests(q, 0, z);
	return q;
}

static void cerrar_frame(qword_t *dmatag, qword_t *q, packet_t *paquete)
{
	q = draw_finish(q);
	DMATAG_END(dmatag, (q - paquete->data) - 1, 0, 0, 0);
	dma_wait_fast();
	dma_channel_send_chain(DMA_CHANNEL_GIF, paquete->data,
	                       q - paquete->data, 0, 0);
	draw_wait_finish();
	graph_wait_vsync();
}

// Un solo fotograma diciendo que se esta cargando, y se manda ANTES de
// abrir el fichero. En la consola la lectura del .ogg son varios segundos
// con el bucle de dibujo parado: sin esto la tele se queda con el ultimo
// fotograma del menu y parece que se ha colgado.
static void pantalla_cargando(framebuffer_t *frame, zbuffer_t *z,
                              packet_t *paquete, const cancion_t *c)
{
	qword_t *dmatag = paquete->data;
	qword_t *q = abrir_frame(dmatag, frame, z);
	color_t blanco, gris;
	char linea[96];

	blanco.r = 0x80; blanco.g = 0x80; blanco.b = 0x80; blanco.a = 0x80; blanco.q = 1.0f;
	gris.r = 0x50; gris.g = 0x50; gris.b = 0x50; gris.a = 0x80; gris.q = 1.0f;

	{
		char corte[64];

		recorte_sjis(corte, sizeof(corte), c->titulo, 40);
		snprintf(linea, sizeof(linea), "Cargando %s", corte);
	}
	q = texto(q, -SEG_X, 20.0f, linea, blanco);
	if (!c->generada) {
		// La ruta entera, recortada por la derecha si no cabe: con carpetas
		// de por medio, saber CUAL fichero se esta abriendo es la mitad de
		// la informacion cuando algo no carga.
		snprintf(linea, sizeof(linea), "%.60s", c->ruta_ogg);
		q = texto(q, -SEG_X, -12.0f, linea, gris);
		q = texto(q, -SEG_X, -60.0f,
		          "Leyendo del pen: puede tardar unos segundos", gris);
	}

	cerrar_frame(dmatag, q, paquete);
}

// Recoge lo que el lector de fondo llevara adelantado de esta cancion. Si le
// falta poco, espera enseñando el progreso; si ya lo tiene todo, la partida
// arranca sin leer ni un byte del pen.
//
// Es el punto en el que el buffer cambia de dueño: deja de ser del lector y
// pasa a ser el .ogg de la partida, con lo que cargar_audio_de lo encuentra
// cacheado y no vuelve a abrir el fichero.
static void esperar_carga(framebuffer_t *frame, zbuffer_t *z,
                          packet_t *packets[2], const cancion_t *c)
{
	int context = 0;

	// Lo que hay cargado no es de esta cancion (o no hay nada): fuera, y que
	// lo lea cargar_audio_de por el camino de siempre.
	if (carga_buf == NULL || c->generada || c->ruta_ogg[0] == 0 ||
	    strcmp(carga_ruta, c->ruta_ogg) != 0) {
		parar_carga();
		return;
	}

	// La espera se corta si deja de AVANZAR, no por tiempo total: un pen lento
	// puede tardar quince segundos legitimamente en un fichero grande, y
	// cortarle por eso seria peor que esperar. Lo que no puede pasar es
	// quedarse aqui para siempre si la lectura se atasca, porque esta pantalla
	// no responde a nada.
	{
		long ultimo = -1;
		unsigned int t_avance = cpu_ticks();

		while (carga_viva) {
			qword_t *dmatag;
			qword_t *q;
			color_t blanco, amarillo;
			char linea[80];
			int pct;

			if (carga_validos != ultimo) {
				ultimo   = carga_validos;
				t_avance = cpu_ticks();
			} else if ((unsigned int)(cpu_ticks() - t_avance) >
			           (unsigned int)(TICKS_POR_SEG * 5)) {
				LOG("AVISO: la carga se atasco en %d KB, se lee del tiron\n",
				    (int)(carga_validos / 1024));
				break;
			}

			dmatag = packets[context]->data;
			q = abrir_frame(dmatag, frame, z);
			pct = (carga_total > 0)
			      ? (int)((long long)carga_validos * 100 / carga_total) : 0;

			blanco.r = 0x80; blanco.g = 0x80; blanco.b = 0x80;
			blanco.a = 0x80; blanco.q = 1.0f;
			amarillo.r = 0x80; amarillo.g = 0x80; amarillo.b = 0x00;
			amarillo.a = 0x80; amarillo.q = 1.0f;

			recorte_sjis(linea, sizeof(linea), c->titulo, 34);
			q = texto(q, -SEG_X, 40.0f, linea, blanco);
			snprintf(linea, sizeof(linea), "Cargando  %d%%", pct);
			q = texto(q, -SEG_X, 0.0f, linea, amarillo);

			cerrar_frame(dmatag, q, packets[context]);
			context ^= 1;
		}
	}

	if (!carga_viva && carga_validos >= carga_total && carga_total > 0) {
		// Cambio de dueño. El buffer NO se libera: se lo queda el cache del
		// .ogg, y por eso aqui se pone carga_buf a NULL antes de nada mas.
		if (ogg_buffer != NULL) free(ogg_buffer);
		ogg_buffer    = carga_buf;
		ogg_cache_tam = carga_total;
		snprintf(ogg_en_cache, sizeof(ogg_en_cache), "%s", carga_ruta);

		carga_buf     = NULL;
		carga_total   = 0;
		carga_validos = 0;
		carga_ruta[0] = 0;
		LOG("Carga: la cancion ya estaba en RAM al entrar\n");
	} else {
		// Se quedo a medias (lectura corta o abortada): se tira y que lo lea
		// cargar_audio_de entero, que es el camino probado.
		parar_carga();
	}
}

// Primera parada del catalogo con esa chart generada, o -1. Hace falta para
// poder mandar al jugador derecho al metronomo la primera vez.
static int indice_generada(int tipo)
{
	int i;

	for (i = 0; i < n_canciones; i++)
		if (canciones[i].generada == tipo) return i;
	return -1;
}

// Solo la primera vez que se enciende en un equipo, o sea cuando no hay
// fichero de ajustes en el pen. Devuelve 1 si se quiere calibrar ahora.
//
// Se pregunta en vez de mandar directo al metronomo porque la calibracion no
// es obligatoria para jugar: con offset 0 el juego funciona, solo que quien
// tenga una tele con retraso lo notara. Y se pregunta UNA vez: diga lo que
// diga, despues hay fichero y no vuelve a salir.
static int pantalla_bienvenida(framebuffer_t *frame, zbuffer_t *z,
                               packet_t *packets[2])
{
	static const char *opciones[2] = {
		"CALIBRAR AHORA con el metronomo",
		"JUGAR SIN CALIBRAR"
	};
	int sel = 0;
	int context = 0;
	unsigned short prev_btns = 0x0000;
	color_t blanco, amarillo, gris;

	blanco.r = 0x80; blanco.g = 0x80; blanco.b = 0x80; blanco.a = 0x80; blanco.q = 1.0f;
	amarillo.r = 0x80; amarillo.g = 0x80; amarillo.b = 0x00; amarillo.a = 0x80; amarillo.q = 1.0f;
	gris.r = 0x50; gris.g = 0x50; gris.b = 0x50; gris.a = 0x80; gris.q = 1.0f;

	dma_wait_fast();

	for (;;) {
		struct padButtonStatus buttons;
		unsigned short btns;
		qword_t *dmatag, *q;
		int arriba, abajo, elegir, i;
		char linea[80];

		if (padRead(0, 0, &buttons) != 0) btns = buttons.btns;
		else                              btns = 0xFFFF;

		arriba = !(btns & PAD_L1) && (prev_btns & PAD_L1);
		abajo  = !(btns & PAD_R1) && (prev_btns & PAD_R1);
		elegir = ((~btns) & prev_btns & BOTONES_DON) != 0;

		if (arriba || abajo) sel ^= 1;
		if (elegir) return (sel == 0);

		prev_btns = btns;

		dmatag = packets[context]->data;
		q = abrir_frame(dmatag, frame, z);

		q = texto(q, -SEG_X, SEG_Y, "PRIMERA VEZ EN ESTE EQUIPO", amarillo);

		q = texto(q, -SEG_X, SEG_Y - 44.0f,
		          "No hay ajustes tuyos en el pen todavia.", blanco);
		q = texto(q, -SEG_X, SEG_Y - 76.0f,
		          "Entre el sonido que sale y la imagen que ves hay un", gris);
		q = texto(q, -SEG_X, SEG_Y - 100.0f,
		          "retraso que depende de tu tele y de tu equipo. El", gris);
		q = texto(q, -SEG_X, SEG_Y - 124.0f,
		          "metronomo lo mide en un minuto y se guarda en el pen,", gris);
		q = texto(q, -SEG_X, SEG_Y - 148.0f,
		          "junto a las canciones. Esto no se vuelve a preguntar.", gris);

		for (i = 0; i < 2; i++) {
			snprintf(linea, sizeof(linea), "%c %s",
			         (i == sel) ? '>' : ' ', opciones[i]);
			q = texto(q, -SEG_X + 40.0f, -40.0f - i * 34.0f, linea,
			          (i == sel) ? amarillo : blanco);
		}

		q = texto(q, -SEG_X, -SEG_Y + 22.0f,
		          "AZUL (L1/R1) cambia   ROJO elige", gris);

		cerrar_frame(dmatag, q, packets[context]);
		context ^= 1;
	}
}

//---------------------------------------------------------------------
// Opciones
//---------------------------------------------------------------------
// Se abre con START desde la lista de canciones. Lo que puede pedirle al
// selector cuando se cierra:
#define ACC_NADA       0
#define ACC_METRONOMO  1
#define ACC_PRUEBA     2

#define OPC_MUSICA   0
#define OPC_SONIDO   1
#define OPC_NOTAS    2   // disco o anillo
// El temblor NO esta aqui: se toca dentro de la cancion con SELECT y L2/R2,
// que es el unico sitio donde se ve lo que hace mientras se cambia.
#define OPC_BORRAR   3
#define OPC_METRO    4
#define OPC_PRUEBA   5
#define OPC_SALIR    6
#define N_OPCIONES   7

// La barra de rayitas, tal cual se enseña: llenas con '|' y vacias con '*'.
static void barra_volumen(char *dst, size_t n, const char *etiqueta, int valor)
{
	char b[VOL_PASOS + 1];
	int i;

	for (i = 0; i < VOL_PASOS; i++) b[i] = (i < valor) ? '|' : '*';
	b[VOL_PASOS] = '\0';

	snprintf(dst, n, "%-7s %s", etiqueta, b);
}

static int pantalla_opciones(framebuffer_t *frame, zbuffer_t *z,
                             packet_t *packets[2])
{
	int sel = 0;
	int context = 0;
	int cambiado = 0;      // hay ajustes que todavia no estan en el pen
	int confirmando = 0;   // 0 = menu, 1 = "¿seguro?" del borrado
	int conf_sel = 1;      // y ahi se empieza sobre el NO, no sobre el SI
	unsigned short prev_btns = 0x0000;
	color_t blanco, amarillo, gris, fondo;

	blanco.r = 0x80; blanco.g = 0x80; blanco.b = 0x80; blanco.a = 0x80; blanco.q = 1.0f;
	amarillo.r = 0x80; amarillo.g = 0x80; amarillo.b = 0x00; amarillo.a = 0x80; amarillo.q = 1.0f;
	gris.r = 0x50; gris.g = 0x50; gris.b = 0x50; gris.a = 0x80; gris.q = 1.0f;
	fondo.r = 0x14; fondo.g = 0x14; fondo.b = 0x28; fondo.a = 0x80; fondo.q = 1.0f;

	dma_wait_fast();

	for (;;) {
		struct padButtonStatus buttons;
		unsigned short btns;
		qword_t *dmatag, *q;
		int arriba, abajo, rojo_izq, rojo_der, elegir, salir = 0;
		int accion = ACC_NADA;
		int i;
		char linea[80];

		if (padRead(0, 0, &buttons) != 0) btns = buttons.btns;
		else                              btns = 0xFFFF;

		arriba   = !(btns & PAD_L1) && (prev_btns & PAD_L1);
		abajo    = !(btns & PAD_R1) && (prev_btns & PAD_R1);
		rojo_izq = ((~btns) & prev_btns & BOTONES_DON_IZQ) != 0;
		rojo_der = ((~btns) & prev_btns & BOTONES_DON_DER) != 0;
		elegir   = rojo_izq || rojo_der;

		if (confirmando) {
			if (arriba || abajo) conf_sel ^= 1;
			if (elegir) {
				if (conf_sel == 0) {
					int ok_perfil, borradas;

					// Un fotograma avisando ANTES de empezar. Recorrer hasta
					// 32 carpetas del pen son 64 llamadas al sistema de
					// ficheros, y en USB 1.1 eso puede irse a segundos: sin
					// esto la pantalla se queda clavada en el "¿seguro?" y no
					// hay forma de distinguirlo de un cuelgue.
					dmatag = packets[context]->data;
					q = abrir_frame(dmatag, frame, z);
					q = build_rect(q, -250.0f, -130.0f, 500.0f, 300.0f, fondo);
					q = texto(q, -230.0f, 20.0f,
					          "Borrando perfil y puntuaciones...", blanco);
					cerrar_frame(dmatag, q, packets[context]);
					context ^= 1;

					// El menu promete "y puntuaciones", asi que se borran.
					ok_perfil = (borrar_perfil() == 0);
					borradas  = borrar_todas_las_puntuaciones();

					// Solo se pisa el mensaje si el perfil se borro: si
					// fallo, el que hay que ver es el suyo.
					if (ok_perfil)
						snprintf(config_estado, sizeof(config_estado),
						         "borrado, %d canciones", borradas);
				}
				// Se acepte o no, lo que hubiera pendiente de guardar deja de
				// estarlo: si no, al salir se volveria a escribir el fichero
				// que se acaba de borrar.
				cambiado    = 0;
				confirmando = 0;
				conf_sel    = 1;
			}
		} else {
			if (arriba) sel = (sel + N_OPCIONES - 1) % N_OPCIONES;
			if (abajo)  sel = (sel + 1) % N_OPCIONES;

			if (sel == OPC_MUSICA && (rojo_izq || rojo_der)) {
				vol_musica += rojo_der ? 1 : -1;
				if (vol_musica < 0)         vol_musica = 0;
				if (vol_musica > VOL_PASOS) vol_musica = VOL_PASOS;
				aplicar_volumen_musica();
				cambiado = 1;
			} else if (sel == OPC_NOTAS && (rojo_izq || rojo_der)) {
				estilo_nota = (estilo_nota == ESTILO_DISCO) ? ESTILO_ANILLO
				                                            : ESTILO_DISCO;
				cambiado = 1;
			} else if (sel == OPC_SONIDO && (rojo_izq || rojo_der)) {
				vol_sonido += rojo_der ? 1 : -1;
				if (vol_sonido < 0)         vol_sonido = 0;
				if (vol_sonido > VOL_PASOS) vol_sonido = VOL_PASOS;
				aplicar_volumen_sonido();
				// Y suena un golpe, que es la unica forma de oir lo que estas
				// moviendo: en el menu no hay musica de fondo con la que
				// comparar. Se puede llamar a audsrv desde aqui porque en el
				// selector no hay hilo de audio vivo.
				audsrv_ch_play_adpcm(-1, &muestra[SFX_DON]);
				cambiado = 1;
			} else if (elegir) {
				switch (sel) {
				case OPC_BORRAR: confirmando = 1; conf_sel = 1; break;
				case OPC_METRO:  accion = ACC_METRONOMO; salir = 1; break;
				case OPC_PRUEBA: accion = ACC_PRUEBA;    salir = 1; break;
				case OPC_SALIR:  salir = 1; break;
				default: break;
				}
			}

			// START cierra, igual que abre.
			if (!(btns & PAD_START) && (prev_btns & PAD_START)) salir = 1;
		}

		if (salir) {
			// El fichero se escribe UNA vez, al cerrar, y no en cada rayita:
			// escribir en el pen cuesta y no hace falta hacerlo diez veces
			// mientras alguien sube el volumen.
			if (cambiado) guardar_config();
			return accion;
		}

		prev_btns = btns;

		dmatag = packets[context]->data;
		q = abrir_frame(dmatag, frame, z);

		// El panel va antes que el texto: el texto estampa la Z maxima y
		// cualquier figura mandada despues se perderia (ver texto()).
		q = build_rect(q, -250.0f, -130.0f, 500.0f, 300.0f, fondo);

		if (confirmando) {
			q = texto(q, -230.0f, 140.0f, "BORRAR PERFIL", amarillo);
			q = texto(q, -230.0f, 96.0f,
			          "Se pierden la calibracion, los volumenes", blanco);
			q = texto(q, -230.0f, 68.0f,
			          "y las puntuaciones de todas las canciones.", blanco);
			q = texto(q, -230.0f, 24.0f, "Esto no se puede deshacer.", gris);

			snprintf(linea, sizeof(linea), "%c SI, BORRAR",
			         (conf_sel == 0) ? '>' : ' ');
			q = texto(q, -190.0f, -30.0f, linea,
			          (conf_sel == 0) ? amarillo : blanco);
			snprintf(linea, sizeof(linea), "%c NO, DEJARLO COMO ESTA",
			         (conf_sel == 1) ? '>' : ' ');
			q = texto(q, -190.0f, -64.0f, linea,
			          (conf_sel == 1) ? amarillo : blanco);
		} else {
			q = texto(q, -230.0f, 140.0f, "OPCIONES", amarillo);

			for (i = 0; i < N_OPCIONES; i++) {
				color_t col = (i == sel) ? amarillo : blanco;
				char cursor = (i == sel) ? '>' : ' ';
				char texto_fila[64];

				switch (i) {
				case OPC_MUSICA:
					barra_volumen(texto_fila, sizeof(texto_fila),
					              "Musica:", vol_musica);
					break;
				case OPC_SONIDO:
					barra_volumen(texto_fila, sizeof(texto_fila),
					              "Sonido:", vol_sonido);
					break;
				case OPC_NOTAS:
					snprintf(texto_fila, sizeof(texto_fila),
					         "Notas: %s",
					         (estilo_nota == ESTILO_DISCO) ? "circulo lleno"
					                                       : "anillo");
					break;
				case OPC_BORRAR:
					snprintf(texto_fila, sizeof(texto_fila),
					         "Borrar perfil (y puntuaciones)");
					break;
				case OPC_METRO:
					snprintf(texto_fila, sizeof(texto_fila),
					         "Metronomo (calibrar)");
					break;
				case OPC_PRUEBA:
					snprintf(texto_fila, sizeof(texto_fila),
					         "Nivel de prueba");
					break;
				default:
					snprintf(texto_fila, sizeof(texto_fila),
					         "Volver al selector");
					break;
				}

				snprintf(linea, sizeof(linea), "%c %s", cursor, texto_fila);
				// 30 y no 34 de separacion: con la fila de notas son siete, y a
				// 34 la ultima se junta con la linea de ayuda de abajo.
				q = texto(q, -230.0f, 96.0f - i * 30.0f, linea, col);
			}

			// La ayuda cambia con la fila porque los rojos hacen dos cosas
			// distintas: en una barra suben y bajan, en lo demas eligen.
			if (sel == OPC_MUSICA || sel == OPC_SONIDO || sel == OPC_NOTAS)
				q = texto(q, -230.0f, -110.0f,
				          "AZUL mueve   ROJO izq baja / der sube", gris);
			else
				q = texto(q, -230.0f, -110.0f,
				          "AZUL mueve   ROJO elige   START cierra", gris);
		}

		cerrar_frame(dmatag, q, packets[context]);
		context ^= 1;
	}
}

//---------------------------------------------------------------------
// Pausa
//---------------------------------------------------------------------
// Para la musica, para el reloj y deja seguir por donde iba. Devuelve
// PAUSA_SIGUE o PAUSA_SALIR.
//
// Quien la ejecuta de verdad es el hilo de audio (ver la seccion de pausa
// dentro de hilo_audio): es el unico que puede tocar audsrv mientras la
// cancion suena. Aqui solo se pide, se espera el acuse y se dibuja.
static int pantalla_pausa(framebuffer_t *frame, zbuffer_t *z,
                          packet_t *packets[2], const cancion_t *can)
{
	// Tres y no dos. "Terminar" y "volver al menu" parecen lo mismo y no lo
	// son: terminar saca el resumen, y eso es lo unico que permite calibrar
	// sin tragarse los dos minutos enteros del metronomo. Juntarlas obligaria
	// a que una de las dos hiciera algo distinto de lo que dice su nombre.
	// "Reiniciar" va la segunda y no la ultima a proposito: es la que se pide
	// a media cancion cuando algo ha salido mal, o sea con prisa, y las dos de
	// abajo son las de dejarlo. Repetir NO vuelve a leer el .ogg del pen:
	// cargar_audio_de lo tiene cacheado en RAM, asi que empieza en el acto.
	static const char *opciones[4] = {
		"REANUDAR",
		"REINICIAR LA CANCION",
		"TERMINAR Y VER RESULTADOS",
		"VOLVER AL MENU"
	};
	int sel = 0;
	int context = 0;
	int frio;
	unsigned int t0;
	// 0x0000 = "todo pulsado": en libpad la logica va invertida, asi que el
	// START con el que se llega aqui no cuenta hasta soltarlo.
	unsigned short prev_btns = 0x0000;
	color_t blanco, amarillo, gris;

	blanco.r = 0x80; blanco.g = 0x80; blanco.b = 0x80; blanco.a = 0x80; blanco.q = 1.0f;
	amarillo.r = 0x80; amarillo.g = 0x80; amarillo.b = 0x00; amarillo.a = 0x80; amarillo.q = 1.0f;
	gris.r = 0x50; gris.g = 0x50; gris.b = 0x50; gris.a = 0x80; gris.q = 1.0f;

	pausa_pedida = 1;

	// Esperar a que el hilo conteste. Aqui SI se puede girar en vacio: el
	// hilo de audio tiene mas prioridad, asi que desaloja a este y avanza.
	// Se acota igual, y se mira audio_terminado ademas de pausa_activa: si
	// el hilo ya habia acabado (una chart que dura mas que su .ogg, que es
	// un caso que el propio motor avisa por consola) no hay nadie que
	// conteste y esto se quedaria el segundo entero esperando.
	t0 = cpu_ticks();
	while (!pausa_activa && !audio_terminado &&
	       (unsigned int)(cpu_ticks() - t0) < (unsigned int)TICKS_POR_SEG)
		;

	// Pausa "en frio": no hay hilo de audio al que pedirle nada, asi que el
	// reloj se para desde aqui. Sin esto la pausa no congelaria nada y al
	// volver saldrian de golpe todas las notas que hubieran pasado.
	frio = !pausa_activa;
	if (frio) {
		pausa_pedida = 0;
		congelar_reloj();
	}

	for (;;) {
		struct padButtonStatus buttons;
		unsigned short btns;
		qword_t *dmatag, *q;
		int arriba, abajo, elegir, i;
		char linea[80];

		if (padRead(0, 0, &buttons) != 0) btns = buttons.btns;
		else                              btns = 0xFFFF;

		// Mismo mapeo que el menu: los bordes azules mueven, los parches
		// rojos eligen. START tambien reanuda, que es lo que espera
		// cualquiera que la haya abierto con START.
		arriba = !(btns & PAD_L1) && (prev_btns & PAD_L1);
		abajo  = !(btns & PAD_R1) && (prev_btns & PAD_R1);
		elegir = ((~btns) & prev_btns & BOTONES_DON) != 0;

		if (arriba) sel = (sel + 3) % 4;
		if (abajo)  sel = (sel + 1) % 4;

		// START reanuda, que es lo que espera quien la abrio con START.
		if (!(btns & PAD_START) && (prev_btns & PAD_START)) { sel = 0; elegir = 1; }

		if (elegir && sel == 1) return PAUSA_REINICIAR;
		if (elegir && sel == 2) return PAUSA_RESULTADOS;
		if (elegir && sel == 3) return PAUSA_MENU;
		if (elegir && sel == 0) break;

		prev_btns = btns;

		dmatag = packets[context]->data;
		q = abrir_frame(dmatag, frame, z);

		q = texto(q, -SEG_X, SEG_Y, "PAUSA", amarillo);
		recorte_sjis(linea, sizeof(linea), can->titulo, 40);
		q = texto(q, -SEG_X, SEG_Y - 40.0f, linea, blanco);

		for (i = 0; i < 4; i++) {
			snprintf(linea, sizeof(linea), "%c %s",
			         (i == sel) ? '>' : ' ', opciones[i]);
			q = texto(q, -SEG_X + 40.0f, 56.0f - i * 34.0f, linea,
			          (i == sel) ? amarillo : blanco);
		}

		q = texto(q, -SEG_X, -SEG_Y + 22.0f,
		          "AZUL (L1/R1) cambia   ROJO elige   START reanuda", gris);
		if (frio)
			q = texto(q, -SEG_X, -SEG_Y,
			          "(la cancion ya habia terminado)", gris);

		cerrar_frame(dmatag, q, packets[context]);
		context ^= 1;
	}

	//--- Cuenta atras ---
	//
	// Va ANTES de despertar al hilo de audio, no despues: asi la musica
	// arranca justo cuando se acaba la cuenta. Y con el reloj todavia
	// congelado, o sea que el juego no avanza ni un milisegundo mientras
	// tanto.
	//
	// Se mide con cpu_ticks y no contando fotogramas porque un fotograma
	// dura 20 ms en PAL y 16,7 en NTSC: contando frames, la misma cuenta
	// duraria tres segundos aqui y dos y medio en otra consola.
	{
		unsigned int t_ini = cpu_ticks();

		for (;;) {
			struct padButtonStatus b2;
			unsigned int pasado = (unsigned int)(cpu_ticks() - t_ini);
			int quedan = 3 - (int)(pasado / TICKS_POR_SEG);
			qword_t *dmatag, *q;
			char linea[64];

			if (quedan <= 0) break;

			// Se sigue leyendo el mando aunque no se use: si no, libpad se
			// queda sin refrescar tres segundos y la primera lectura de
			// despues puede salir mala. El (void) es a proposito: en todo el
			// resto del fichero el retorno de padRead SE MIRA, porque una
			// lectura fallida no toca la estructura y deja basura; aqui el
			// valor no se usa, y marcarlo evita que alguien copie de aqui la
			// forma sin comprobar a un sitio donde si importa.
			(void)padRead(0, 0, &b2);

			dmatag = packets[context]->data;
			q = abrir_frame(dmatag, frame, z);

			recorte_sjis(linea, sizeof(linea), can->titulo, 40);
			q = texto(q, -SEG_X, SEG_Y, linea, blanco);
			snprintf(linea, sizeof(linea), "REANUDANDO EN %d", quedan);
			q = texto(q, -60.0f, 20.0f, linea, amarillo);

			cerrar_frame(dmatag, q, packets[context]);
			context ^= 1;
		}
	}

	//--- Reanudar ---
	if (!frio) {
		pausa_pedida = 0;
		if (hilo_audio_id >= 0) WakeupThread(hilo_audio_id);

		// No se puede dejar correr al juego hasta que el hilo republique el
		// reloj: lo que hay publicado es de antes de la pausa.
		t0 = cpu_ticks();
		while (pausa_activa && !audio_terminado &&
		       (unsigned int)(cpu_ticks() - t0) < (unsigned int)TICKS_POR_SEG)
			;
		if (pausa_activa || reloj_congelado) {
			// Si no contesto, el reloj NO se puede quedar parado: la partida
			// se quedaria congelada para siempre y eso es peor que un salto.
			LOG("AVISO: el audio no reanudo, se sigue sin el\n");
			pausa_activa = 0;
			descongelar_reloj(reloj_ms_pausa);
		}
	} else {
		descongelar_reloj(reloj_ms_pausa);
	}

	return PAUSA_SIGUE;
}

//---------------------------------------------------------------------
// Selector
//---------------------------------------------------------------------
// Dos pantallas: primero la lista de canciones y luego la de dificultad.
// Estan separadas porque el mando aqui es un tambor, y un tambor solo tiene
// cuatro entradas: dos bordes azules y dos parches rojos. Con una sola
// pantalla harian falta cuatro direcciones (arriba/abajo para la cancion,
// izquierda/derecha para la dificultad) y no las hay.
//
// Y la cruceta ya no puede navegar: IZQUIERDA y ABAJO son parches rojos
// (ver BOTONES_DON), asi que moverse con ellos seria elegir a la vez.
#define PANT_CANCION  0
#define PANT_CURSO    1

// La opcion "atras" de la pantalla de dificultad va detras de los cursos, en
// la misma rueda.
#define OPC_ATRAS     N_CURSOS

// Siguiente parada de esa rueda, saltandose los cursos que la cancion no
// trae. Da la vuelta a proposito: con solo dos botones para moverse, poder
// volver por el otro lado es lo que evita tener que recorrer la fila entera
// para llegar a lo que esta justo al lado.
static int opcion_curso(const cancion_t *c, int desde, int paso)
{
	int k = desde, v;

	for (v = 0; v <= N_CURSOS; v++) {
		k += paso;
		if (k < 0)         k = OPC_ATRAS;
		if (k > OPC_ATRAS) k = 0;
		if (k == OPC_ATRAS || c->nivel[k] >= 0) return k;
	}
	return desde;
}

// Vuelve cuando se ha elegido cancion Y dificultad; de aqui no se sale de
// ninguna otra forma.
static void menu(framebuffer_t *frame, zbuffer_t *z, packet_t *packets[2],
                 int *cursor, int *curso)
{
	int context = 0;
	int pantalla = PANT_CANCION;
	int sel = 0;
	// Marcas de los cinco cursos de la cancion que se esta mirando, y de cual
	// son. Se leen UNA vez, al entrar en la pantalla de dificultad: hacerlo en
	// cada fotograma seria abrir un fichero del pen cincuenta veces por
	// segundo. El cache no necesita invalidarse a mano porque menu() se llama
	// entera en cada vuelta del ciclo, asi que despues de jugar vuelve vacio y
	// la marca nueva se relee sola.
	int marcas[N_CURSOS];
	int marcas_de = -1;
	int i;
	// La muestra no se lanza en el mismo fotograma en que se entra en la
	// pantalla de dificultad, sino al final de ese fotograma: leer el trozo
	// del pen son segundos en la consola, y asi al menos la pantalla ya esta
	// pintada mientras tanto en vez de quedarse la lista congelada.
	int preview_pendiente = 0;
	// 0x0000 = "todo pulsado". En libpad la logica va invertida, asi que
	// arrancar asi hace que NINGUN boton cuente hasta que se suelte y se
	// vuelva a pulsar. Sin esto, el parche rojo con el que se sale de la
	// pantalla de resultados seguiria pulsado al llegar aqui y el menu
	// entraria en la cancion en el primer fotograma.
	unsigned short prev_btns = 0x0000;
	color_t blanco, amarillo, gris;
#if AUTOCICLO
	int frames_auto = 0;
#endif

	blanco.r = 0x80; blanco.g = 0x80; blanco.b = 0x80; blanco.a = 0x80; blanco.q = 1.0f;
	amarillo.r = 0x80; amarillo.g = 0x80; amarillo.b = 0x00; amarillo.a = 0x80; amarillo.q = 1.0f;
	gris.r = 0x50; gris.g = 0x50; gris.b = 0x50; gris.a = 0x80; gris.q = 1.0f;

	// A -1 desde ya: solo se llenan al entrar en la pantalla de dificultad, y
	// un array de pila sin tocar enseñaria basura como si fueran puntuaciones.
	for (i = 0; i < N_CURSOS; i++) marcas[i] = -1;

	// El cursor puede venir apuntando a una generada, que es lo que pasa al
	// volver de jugar el metronomo desde las opciones. Esas ya no estan en la
	// lista, asi que se vuelve al principio.
	if (*cursor >= n_visibles) *cursor = 0;

	dma_wait_fast();

	for (;;) {
		struct padButtonStatus buttons;
		unsigned short btns;
		qword_t *dmatag, *q;
		cancion_t *c;
		char linea[80];
		char corte[64];
		int k, primera;
		int arriba, abajo, elegir;

		// padRead devuelve 0 si no pudo leer, y en ese caso NO toca la
		// estructura: usarla igual seria leer basura de la pila.
		if (padRead(0, 0, &buttons) != 0) btns = buttons.btns;
		else                              btns = 0xFFFF;

		c = &canciones[*cursor];

		// Mapeo del tambor: bordes azules para moverse, parches rojos para
		// elegir. Se mira el flanco de CUALQUIER boton rojo, no de uno
		// concreto (ver BOTONES_DON).
		arriba = !(btns & PAD_L1) && (prev_btns & PAD_L1);
		abajo  = !(btns & PAD_R1) && (prev_btns & PAD_R1);
		elegir = ((~btns) & prev_btns & BOTONES_DON) != 0;

		// Las dos pantallas van en ramas separadas y no en dos bloques
		// seguidos: si no, el mismo golpe rojo elegiria cancion y dificultad
		// en el mismo fotograma y no se veria ni la segunda pantalla.
		if (pantalla == PANT_CANCION) {
			// Con el pen vacio no hay nada que recorrer ni que elegir, y el
			// modulo por n_visibles seria una division por cero.
			if (arriba && n_visibles > 0)
				*cursor = (*cursor + n_visibles - 1) % n_visibles;
			if (abajo && n_visibles > 0)
				*cursor = (*cursor + 1) % n_visibles;
			if (arriba || abajo) c = &canciones[*cursor];

			if (elegir && n_visibles > 0) {
				// Se entra por la dificultad de la cancion anterior si esta
				// tambien la trae; si no, por la primera que tenga.
				sel = (c->nivel[*curso] >= 0) ? *curso
				                              : opcion_curso(c, -1, +1);
				pantalla = PANT_CURSO;

				// Las marcas se leen del pen AQUI, antes de que arranque la
				// muestra: dos cosas leyendo del pen a la vez es justo lo que
				// no puede pasar (una sola tanda de llamadas por el SIF).
				if (marcas_de != *cursor) {
					leer_puntos_todos(c, marcas);
					marcas_de = *cursor;
				}

				preview_pendiente = 1;
			}
		} else {
			if (arriba) sel = opcion_curso(c, sel, -1);
			if (abajo)  sel = opcion_curso(c, sel, +1);

			if (elegir) {
				if (sel == OPC_ATRAS) {
					// Atras SI aborta la lectura: o no se va a jugar nada, o
					// va a ser otra cancion. Y libera, que son megas.
					parar_carga();
					preview_pendiente = 0;
					pantalla = PANT_CANCION;
				} else {
					// La muestra se para (el hilo de la cancion va a coger
					// audsrv y no puede haber dos dueños), pero la lectura NO
					// se aborta: lo que lleve leido vale tal cual, porque el
					// trozo de la muestra es el principio del mismo fichero.
					// De eso vive esperar_carga.
					parar_preview();
					*curso = sel;
					return;
				}
			}
		}

		// START abre las opciones, y solo desde la lista: en la pantalla de
		// dificultad no hace nada.
		//
		// Lo que NUNCA puede hacer es salir del bucle de main. Antes lo hacia,
		// y main cerraba audsrv y se iba a SleepThread: en pantalla eso es
		// exactamente un cuelgue, con el ultimo fotograma clavado y sin
		// responder. Una PS2 no tiene a donde "salir": se apaga o se resetea.
		// SELECT vuelve a mirar el pen, y solo desde la lista. Sirve para
		// enchufarlo con el juego ya arrancado, o para recoger canciones que
		// se hayan copiado despues.
		//
		// El aviso se pinta y se MANDA antes de escanear: si el pen no esta,
		// catalogar_canciones se pasa hasta ocho segundos esperandolo, y como
		// todo esto va en el hilo del menu la pantalla se queda quieta. Sin el
		// aviso, eso se ve exactamente igual que un cuelgue.
		if (pantalla == PANT_CANCION &&
		    !(btns & PAD_SELECT) && (prev_btns & PAD_SELECT)) {
			qword_t *dt2, *q2;

			dt2 = packets[context]->data;
			q2 = abrir_frame(dt2, frame, z);
			q2 = texto(q2, -SEG_X, SEG_Y, "BUSCANDO EL PEN...", amarillo);
			q2 = texto(q2, -SEG_X, SEG_Y - 40.0f,
			           "Si no esta puesto, esto tarda unos segundos.", gris);
			cerrar_frame(dt2, q2, packets[context]);
			context ^= 1;

			// Rehace el catalogo entero: empieza poniendo n_canciones a cero y
			// ya se encarga de ordenar y de volver a poner las generadas al
			// final, asi que no hay nada que limpiar antes. La lectura de
			// fondo si hay que cortarla: va a leer del pen igual que el
			// escaneo, y solo puede haber uno.
			parar_carga();
			catalogar_canciones();

			// El cursor puede haberse quedado apuntando a una cancion que ya
			// no esta, o a un hueco detras de la lista nueva.
			if (*cursor >= n_visibles) *cursor = 0;
			marcas_de = -1;          // las marcas cacheadas son de la lista vieja
			pantalla  = PANT_CANCION;
			preview_pendiente = 0;

			prev_btns = 0x0000;
			continue;
		}

		if (pantalla == PANT_CANCION &&
		    !(btns & PAD_START) && (prev_btns & PAD_START)) {
			int acc = pantalla_opciones(frame, z, packets);

			if (acc != ACC_NADA) {
				int g = indice_generada((acc == ACC_METRONOMO)
				                        ? GEN_METRONOMO : GEN_PRUEBA);
				if (g >= 0) {
					// Se sale derecho a jugar, asi que audsrv tiene que
					// quedar libre y el pen tambien. Aqui no deberia haber
					// nada de eso vivo (las opciones solo se abren desde la
					// lista), pero esto es lo que hace que siga siendo verdad
					// si algun dia se abren desde otro sitio.
					parar_carga();
					*cursor = g;
					*curso  = 0;   // las generadas solo tienen un curso
					return;
				}
			}

			// Lo que siguiera pulsado al cerrar las opciones no cuenta, o el
			// golpe con el que se eligio "Volver" seleccionaria cancion.
			prev_btns = 0x0000;
			continue;
		}

		prev_btns = btns;

#if AUTOCICLO
		if (++frames_auto > 50) {
			// Cambia de cancion en cada vuelta: asi se recorre de verdad el
			// camino de liberar el .ogg anterior y pedir el siguiente, que
			// es donde se veria si la RAM se va comiendo.
			*cursor = (AUTOCICLO_CANCION >= 0)
			          ? (AUTOCICLO_CANCION % n_canciones)
			          : (*cursor + 1) % n_canciones;
			c = &canciones[*cursor];
			if (AUTOCICLO_CURSO >= 0 && c->nivel[AUTOCICLO_CURSO] >= 0)
				*curso = AUTOCICLO_CURSO;
			if (c->nivel[*curso] < 0) *curso = curso_valido(c, -1, 1);
			return;
		}
#endif

		dmatag = packets[context]->data;
		q = abrir_frame(dmatag, frame, z);

		if (pantalla == PANT_CANCION) {
			q = texto(q, -SEG_X, SEG_Y, "SELECCIONA CANCION", amarillo);

			// Ventana desplazable, con el cursor centrado mientras se pueda.
			// Hacen falta dos cosas de aqui: que la lista no se salga de la
			// zona segura, y que el packet no se desborde. A ~8 qwords por
			// caracter, 32 canciones de 30 letras se irian a ~7700 de los
			// 8192: demasiado cerca para dejarlo suelto.
			primera = *cursor - FILAS_MENU / 2;
			if (primera > n_visibles - FILAS_MENU) primera = n_visibles - FILAS_MENU;
			if (primera < 0) primera = 0;

			for (i = primera; i < n_visibles && i < primera + FILAS_MENU; i++) {
				// El cursor se marca con un signo delante y con el color, no
				// moviendo la linea: si se desplazara, el menu bailaria.
				// %-28.28s y no %-28s: recortado al ancho de la columna, para
				// que un titulo largo no se salga de la zona segura.
				// Recorte propio y no "%.28s": el printf corta por bytes y
				// puede partir un kanji. Y el relleno a 28 sobraba, porque
				// detras del titulo no va nada.
				recorte_sjis(corte, sizeof(corte), canciones[i].titulo, 28);
				snprintf(linea, sizeof(linea), "%c %s",
				         (i == *cursor) ? '>' : ' ', corte);
				q = texto(q, -SEG_X, SEG_Y - 50.0f - (i - primera) * 24.0f, linea,
				          (i == *cursor) ? amarillo : blanco);
			}

			if (n_visibles > FILAS_MENU) {
				snprintf(linea, sizeof(linea), "%d de %d", *cursor + 1, n_visibles);
				q = texto(q, 150.0f, SEG_Y, linea, gris);
			}

			// Sin pen (o con un pen sin .tja) la lista sale vacia, y eso hay
			// que decirlo: el metronomo y el nivel de prueba siguen ahi, pero
			// ahora estan en las opciones y no se ven.
			if (n_visibles == 0) {
				q = texto(q, -SEG_X, SEG_Y - 74.0f,
				          "No hay canciones en el pen.", blanco);
				q = texto(q, -SEG_X, SEG_Y - 106.0f,
				          "START abre las opciones, con el metronomo", gris);
				q = texto(q, -SEG_X, SEG_Y - 130.0f,
				          "y el nivel de prueba.", gris);
			}

			// Las lineas de ayuda van a -SEG_Y + 44 y para arriba, no por
			// debajo de -SEG_Y: ahi se salen de la zona segura y en un tubo
			// se las come el borde. En PCSX2 se ven perfectamente, que es
			// justo lo que hace peligroso este tipo de fallo.
			q = texto(q, -SEG_X, -SEG_Y + 44.0f,
			          "AZUL (L1/R1) mueve   ROJO elige   SELECT relee el pen",
			          gris);
		} else {
			q = texto(q, -SEG_X, SEG_Y, "DIFICULTAD", amarillo);
			recorte_sjis(corte, sizeof(corte), c->titulo, 34);
			q = texto(q, -SEG_X, SEG_Y - 40.0f, corte, blanco);

			// Justo debajo del nombre, la marca del curso que este marcado.
			// Cambia al moverse por la fila, que es de lo que se trata: cada
			// dificultad guarda la suya.
			if (sel != OPC_ATRAS) {
				if (marcas[sel] >= 0)
					snprintf(linea, sizeof(linea), "Mejor: %d puntos",
					         marcas[sel]);
				else
					snprintf(linea, sizeof(linea), "Mejor: sin marca todavia");
				q = texto(q, -SEG_X, SEG_Y - 70.0f, linea,
				          (marcas[sel] >= 0) ? amarillo : gris);
			}

			// Los cursos que no trae la cancion se enseñan igual, en gris:
			// asi se ve de un vistazo que dificultades tiene y cuales faltan.
			// Cinco columnas de 110 px llegan hasta x=170, que cabe en la
			// zona segura; una sexta para el "atras" se saldria, y por eso va
			// en linea aparte aunque sea una parada mas de la misma rueda.
			for (k = 0; k < N_CURSOS; k++) {
				color_t col = (c->nivel[k] < 0) ? gris
				              : (k == sel) ? amarillo : blanco;
				// El signo del cursor va SIEMPRE, tambien en los cursos que
				// no existen: si solo lo llevaran unos, la fila se
				// descuadraria un caracter al moverse.
				if (c->nivel[k] < 0)
					snprintf(linea, sizeof(linea), "%c%s",
					         ' ', nombre_curso[k]);
				else
					snprintf(linea, sizeof(linea), "%c%s %d",
					         (k == sel) ? '>' : ' ', nombre_curso[k],
					         c->nivel[k]);
				q = texto(q, -SEG_X + k * 110.0f, 20.0f, linea, col);
			}

			snprintf(linea, sizeof(linea), "%c VOLVER A LA LISTA",
			         (sel == OPC_ATRAS) ? '>' : ' ');
			q = texto(q, -SEG_X, -60.0f, linea,
			          (sel == OPC_ATRAS) ? amarillo : blanco);

			q = texto(q, -SEG_X, -SEG_Y + 44.0f,
			          "AZUL (L1/R1) mueve   ROJO elige", gris);
		}

		// A ~8,3 px por caracter caben unos 64 en los 540 utiles: la medida
		// de carga necesita linea propia o se sale por la derecha.
		if (ultima_carga[0])
			q = texto(q, -SEG_X, -SEG_Y, ultima_carga, gris);

		// Calibracion, para verla sin entrar en ningun sitio: es un numero
		// que cambia el juicio de todas las notas y conviene saber con cual
		// se esta jugando.
		// %.26s y no %s: la linea de RAM empieza en x=150 y a ~8,3 px por
		// caracter, 45 desde -SEG_X llegan a x~105. Mas largo se pisan.
		snprintf(linea, sizeof(linea), "Calibracion %d ms - %.26s",
		         offset_ms, config_estado);
		q = texto(q, -SEG_X, -SEG_Y + 22.0f, linea, gris);

		// Memoria, para poder mirar de un vistazo si encadenar canciones la
		// va comiendo. No deberia: cargar_audio_de() libera el .ogg anterior
		// ANTES de pedir el siguiente, asi que solo hay uno vivo a la vez.
		{
			struct mallinfo mi = mallinfo();
			// "usados" y "monton", no "libres". El fordblks de newlib es el
			// hueco libre DENTRO del monton, y el monton crece con sbrk
			// cuando hace falta: sale un numero minusculo que asusta y no
			// significa nada. Lo que hay que vigilar es que "usados" no
			// suba cancion tras cancion (fuga) y que "monton" no se separe
			// de "usados" (fragmentacion).
			snprintf(linea, sizeof(linea), "RAM %d KB / monton %d KB",
			         (int)(mi.uordblks / 1024), (int)(mi.arena / 1024));
			q = texto(q, 150.0f, -SEG_Y + 22.0f, linea, gris);
		}

		cerrar_frame(dmatag, q, packets[context]);
		context ^= 1;

		// Al entrar en la pantalla de dificultad se lanza el lector de fondo,
		// UNA vez. Solo cuesta abrir el fichero y mirar cuanto ocupa; leerlo
		// es cosa del otro hilo, que duerme esperando al pen mientras este
		// sigue dibujando.
		//
		// Si la cancion ya esta entera en RAM (se acaba de jugar) no hay nada
		// que leer: intentar_preview tira de ahi directamente.
		if (preview_pendiente) {
			preview_pendiente = 0;
			if (!(ogg_buffer != NULL &&
			      strcmp(ogg_en_cache, c->ruta_ogg) == 0) &&
			    !c->generada && c->ruta_ogg[0] != 0)
				iniciar_carga(c);
		}

		// Y en cada vuelta se mira si ya hay bastante para arrancar la
		// muestra. No hace nada hasta que lo hay, y nada mas una vez.
		if (pantalla == PANT_CURSO) intentar_preview(c);
	}
}

int main(int argc, char *argv[])
{
	static tja_chart_t chart;      // ~50 KB: static, no cabe en la pila
	framebuffer_t frame;
	zbuffer_t z;
	packet_t *packets[2];
	int cursor = 0, curso = 0;
	int vueltas = 0;
	int saltar_menu = 0;
	int fin_render = RENDER_FIN;

	(void)argc; (void)argv;

	cargar_modulos();

	if (audsrv_init() != 0) {
		LOG("audsrv_init fallo: %s\n", audsrv_get_error_string());
		SleepThread();
	}
	audsrv_adpcm_init();

	audsrv_load_adpcm(&muestra[SFX_DON],    don_adp,    size_don_adp);
	audsrv_load_adpcm(&muestra[SFX_BIGDON], bigdon_adp, size_bigdon_adp);
	audsrv_load_adpcm(&muestra[SFX_CANCEL], cancel_adp, size_cancel_adp);

	init_mando();

	// El catalogo se lee antes de que el GS tome el control, que es
	// mientras todavia se puede escribir con scr_printf. Cuando el paso
	// siguiente meta el recorrido de carpetas dentro del menu, esto tendra
	// que pasar por texto() como ya hace pantalla_cargando.
	// Los ajustes viven en el pen, asi que esto va DESPUES de cargar los
	// modulos y antes de que el GS tome el control, que es mientras todavia
	// se puede escribir en pantalla si algo va mal.
	iniciar_ruido();
	cargar_config();
	LOG("Ajustes: %s (offset %d ms, musica %d, sonido %d)\n",
	    config_estado, offset_ms, vol_musica, vol_sonido);

	// Los volumenes van a audsrv nada mas leerlos: si no, la primera cancion
	// sonaria a tope aunque el perfil dijera otra cosa.
	aplicar_volumen();

	catalogar_canciones();

	// Pausa para poder leer en la tele lo que ha encontrado.
	LOG("\nArrancando en 3 segundos...\n");
	sleep(3);

	// A partir de aqui manda el GS: se apaga el texto o el registro del
	// hilo de audio pintaria encima del juego.
	pantalla_texto = 0;

	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);

	init_gs(&frame, &z);
	init_drawing_environment(&frame, &z);

	// KROM son las fuentes de la BIOS: no hay nada que empotrar. El ultimo
	// parametro es la negrita, y va fija a 1: PAL 640x512 es entrelazado y
	// los trazos de un pixel se leen mucho peor. Hubo un rato un conmutador
	// con el TRIANGULO para decidirlo mirandolo; decidido, sobra.
	if (fontx_load("rom0:KROM", &krom, SINGLE_BYTE, 2, 1, 1) < 0) {
		LOG("ERROR: no se pudo cargar rom0:KROM\n");
		SleepThread();
	}
	// La mitad de dos bytes de la misma ROM: kana, simbolos y kanji, unos
	// 105 KB. Si falla NO se para: se sigue sin kanji (ver cargar_krom_kanji).
	hay_kanji = (cargar_krom_kanji(&krom_kanji, 2, 1, 1) == 0);
	if (!hay_kanji)
		LOG("AVISO: sin kanji, los titulos japoneses saldran mal\n");

	// El menu es el packet mas grande que se manda, de ahi el tamaño.
	packets[0] = packet_init(QW_PACKET, PACKET_NORMAL);
	packets[1] = packet_init(QW_PACKET, PACKET_NORMAL);
	if (packets[0] == NULL || packets[1] == NULL) {
		LOG("ERROR: sin memoria para packets de %d qwords\n", QW_PACKET);
		SleepThread();
	}

	curso = (canciones[0].nivel[0] >= 0) ? 0 : curso_valido(&canciones[0], -1, 1);

	// Primera vez en este equipo: sin fichero de ajustes en el pen se ofrece
	// calibrar antes de nada.
	if (!config_existe) {
		int m = indice_generada(GEN_METRONOMO);

		if (pantalla_bienvenida(&frame, &z, packets) && m >= 0) {
			// Derecho al metronomo, sin pasar por el menu. El fichero lo
			// escribe la pantalla de resultados cuando el jugador acepte la
			// medida; si se va sin guardar, la proxima vez se le vuelve a
			// preguntar, que es lo correcto: sigue sin estar calibrado.
			cursor      = m;
			curso       = 0;
			saltar_menu = 1;
		} else {
			// "Jugar sin calibrar" SI escribe el fichero, con el offset de
			// fabrica. Si no, esta pantalla saldria en cada arranque a quien
			// ya ha dicho que no quiere calibrar.
			guardar_config();
		}
	}

	//-----------------------------------------------------------------
	// Ciclo: menu -> cargando -> partida -> resultados -> menu
	//-----------------------------------------------------------------
	for (;;) {
		cancion_t *c;
		int espera;

		// saltar_menu solo lo pone la bienvenida, y dura una vuelta: la
		// primera partida es el metronomo y de ahi en adelante se pasa por
		// el menu como siempre.
		if (!saltar_menu) menu(&frame, &z, packets, &cursor, &curso);
		saltar_menu = 0;

		c = &canciones[cursor];
		{
			// La memoria en cada vuelta, para poder seguirla cancion a
			// cancion. No deberia moverse: solo hay un .ogg vivo a la vez.
			struct mallinfo mi = mallinfo();
			LOG("--- VUELTA %d: '%s' curso %s (RAM %d KB usados, monton %d KB) ---\n",
			    ++vueltas, c->titulo, nombre_curso[curso],
			    (int)(mi.uordblks / 1024), (int)(mi.arena / 1024));
		}

		// El aviso se manda antes de abrir nada: leer del pen para el
		// bucle de dibujo varios segundos.
		pantalla_cargando(&frame, &z, packets[0], c);
		// Recoge lo que el lector de fondo llevara adelantado mientras se
		// elegia la dificultad. Si le dio tiempo a acabar, cargar_audio_de no
		// lee nada: se lo encuentra ya en RAM.
		esperar_carga(&frame, &z, packets, c);
		cargar_audio_de(c);

		// La chart se vuelve a parsear en cada partida porque el catalogo
		// solo guarda la cabecera: una chart entera son ~50 KB y no caben
		// 32 en RAM.
		chart.n_notas = 0;
		if (!c->generada) {
			unsigned char *tja;
			long tam = 0;

			tja = cargar_del_usb(c->ruta_tja, &tam, 0);
			if (tja != NULL) {
				int r = tja_parsear((const char *)tja, tam,
				                    clave_curso[curso], &chart);
				free(tja);
				if (r != 0) {
					LOG("%s: %s\n", c->ruta_tja, tja_error(r));
					chart.n_notas = 0;
				} else {
					LOG("%s [%s]: BPM %d, OFFSET %d ms, %d notas\n",
					    c->titulo, nombre_curso[curso], (int)chart.bpm,
					    (int)(chart.offset * 1000.0f), chart.n_notas);
				}
			}
		}

		// El metronomo se juega en modo calibracion: la ventana de captura
		// se ensancha porque si vas 150 ms desviado, con la normal (108 ms)
		// no engancharias ni una nota y no habria nada que medir. Los 200 ms
		// se quedan por debajo de medio pulso a 140 BPM (214 ms), asi que
		// las ventanas de dos notas seguidas no se pisan.
		modo_calibracion = (c->generada == GEN_METRONOMO);

		// Ventanas de juicio segun el curso: hasta Normal, las anchas.
		{
			const int *v = (curso <= 1) ? ventanas_faciles : ventanas_duras;

			ventana_perfecto = v[0];
			ventana_bueno    = v[1];
			ventana_activa   = modo_calibracion ? VENTANA_CAPTURA_MS : v[2];
		}

		// La marca anterior, ANTES de arrancar el hilo de audio: leer del pen
		// con la cancion ya sonando se nota.
		mejor_puntos = leer_puntos(c, curso);

		// Todo el estado compartido a cero ANTES de crear el hilo.
		preparar_cancion();
		lanzar_hilo_audio();

		espera = 0;
		while (!audio_listo && espera < 300) { nopdelay(); espera++; }
		while (!audio_listo) { nopdelay(); }

		if (audio_terminado && duracion_ms == 0) {
			LOG("El audio no arranco: se vuelve al menu\n");
			detener_audio();
			continue;
		}

		if (chart.n_notas > 0) {
			chart_desde_tja(&chart);
			if (chart.dur_ms > duracion_ms)
				LOG("AVISO: la chart acaba en %d ms y la cancion dura %d ms\n",
				    chart.dur_ms, duracion_ms);
		} else if (c->generada == GEN_PRUEBA) {
			generar_chart_prueba(duracion_ms);
		} else {
			generar_chart(duracion_ms);
		}

		fin_render = render(&frame, &z, packets, c, curso);

		// Y aqui se cierra el ciclo. Parar el audio limpiamente es lo unico
		// que hacia falta para poder volver al menu: audsrv_init y las
		// muestras ADPCM se quedan como estan, solo se rehace el hilo.
		detener_audio();

		// Reiniciar es volver a dar la vuelta con el mismo cursor y el mismo
		// curso, saltandose el selector. Todo lo de arriba se rehace: chart,
		// hilo de audio y estado de la partida. El .ogg no, que ya esta en RAM.
		if (fin_render == RENDER_REINICIAR) saltar_menu = 1;
	}

	/* Del ciclo de arriba no se sale: no hay "salir" en una PS2. */
}
