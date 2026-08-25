# Notas de diseño

Por que esta hecho asi cada trozo del motor: lo que se midio, lo que se probo y
lo que salio mal antes de llegar aqui. Si vas a tocar el codigo, esto es lo que
te ahorra repetir los errores.

Para lo de siempre —que es esto, como se compila, como se juega— mira el
[README](README.md).

Esto empezo siendo el cuaderno de una sola cosa, el reloj de cancion, y de ahi
viene el orden de lo que sigue.

## Paso 1: el reloj de cancion

Junta las dos pruebas que hubo antes de esto —una de GS, mando y sonidos
ADPCM, otra de USB con libvorbisfile y audsrv— y cambia lo unico que impedia
que aquello fuera un juego de ritmo de verdad: **de donde sale la posicion de
las notas**.

| | Antes | Ahora |
|---|---|---|
| Posicion de la nota | `cx -= velocidad` cada frame | `x = X_JUEZ + (t_nota - t_cancion) * PX_POR_MS` |
| Juicio | distancia en pixeles | diferencia en milisegundos |
| Audio | un ADPCM por golpe | musica en streaming + ADPCM por golpe |
| Modulos | `host:audsrv.irx`, sin reset del IOP | todo empotrado en el `.elf`, con reset |

Por que importa: con el metodo viejo, cada frame que se pasa de vsync separa la
musica de las notas **para siempre**. Con el reloj de audio, un frame perdido
hace que las notas den un salto, pero siguen cuadrando con la musica.

## Como se saca la posicion de la cancion

`audsrv` no tiene ninguna funcion que diga "vas por el segundo 12" para audio en
streaming (`audsrv_get_trackpos` es solo para CD). Lo que si tiene es
`audsrv_queued()`, los bytes mandados que aun no han sonado:

```
posicion_ms = (bytes_enviados - audsrv_queued()) * 1000 / (freq * canales * 2)
```

Esa medida cruda tiene tres problemas, y los tres estan resueltos en el codigo:

1. **Solo se refresca cada trozo (~10 ms).** Entre trozo y trozo se interpola con
   `cpu_ticks()`. Error medido: **6-7 ms**.
2. **`audsrv_queued()` devuelve 0 espuriamente** unas 3 veces por segundo, justo
   cuando su buffer circular del IOP da la vuelta. No es un corte de audio: la
   llamada siguiente ya dice ~17000 otra vez. Si se colaba, el reloj pegaba un
   salto de **+106 ms**. Se descarta y se reusa la ultima lectura buena
   (370 descartadas en 120 s).
3. **Diente de sierra de +-5 ms.** El reloj no se cree cada medida: corre solo con
   `cpu_ticks()` y se corrige como mucho `ARRASTRE_MS` (1 ms) por trozo, que a
   ~93 trozos/s dan 93 ms/s de margen. Un fallo suelto solo mueve 1 ms.
4. **El arranque no vale para arrastrarlo.** Mientras la cola se llena,
   `audsrv_wait_audio` no bloquea y se mandan muchos trozos seguidos: la medida
   cruda se queda pegada a cero mientras el tiempo real corre, y llega a
   desviarse **137 ms**. Arrastrar eso a 1 ms por trozo tardaria segundo y medio,
   y durante ese rato las notas del principio de la cancion se juzgarian contra un
   reloj mal (con un `.tja` real que tenga `OFFSET` negativo, eso son varias notas
   imposibles, y parece un fallo del parser). Asi que hasta que la cola no deja de
   crecer, el reloj se **engancha en duro** en vez de arrastrarse.

Queda un **desfase constante** (el DMA al SPU2 va por delante de la cola). Da
igual: se absorbe en `OFFSET_LATENCIA_MS`, que es lo que hay que calibrar en el
paso 2. No esperes que el reloj marque cero exacto.

## Dos cosas que se descubrieron midiendo

### `cpu_ticks()` NO va a `kBUSCLK`

`timer.h` define `kBUSCLK = 147456000`, pero eso es el reloj del **bus**, el que
usan los Timer0-3 del EE. `cpu_ticks()` lee el registro Count del COP0, que en el
R5900 cuenta a la frecuencia del **nucleo**: 294912000 Hz. Medido: ratio 2.0000
exacto. Usar `kBUSCLK` hacia que la interpolacion corriera al doble.

Ademas `cpu_ticks()` es de 32 bits y **da la vuelta cada 29,1 s**. En el reloj no
molesta porque la resta se hace en 32 bits sin signo sobre intervalos de un frame;
en el registro de deriva hubo que acumular en 64 bits.

### El bucle va a 50 Hz, y eso limita el juicio

`640x512` (heredado de `Sounds/`) es modo **PAL**: 50 fps, con picos de 24 ms
entre frames. Como el mando se lee una vez por frame, **la resolucion del juicio
no puede ser mejor que ~20 ms**, y la ventana de perfecto de Taiko es +-25 ms.
Es decir: casi toda la ventana de perfecto se la come el muestreo. No bloquea la
demo, pero hay que tenerlo en cuenta al calibrar la latencia (paso 2), y si se
quiere afinar de verdad habra que leer el mando mas a menudo que una vez por
frame o marcarlo con hora propia.

### `padRead()` no toca la estructura si falla

`padRead` devuelve 0 cuando no ha podido leer, y en ese caso deja
`struct padButtonStatus` **sin tocar**. Declararla dentro del bucle y usarla sin
comprobar el retorno es leer basura de la pila: salian decenas de golpes fantasma
por cancion.

## Por que un hilo de audio, y por que el solo toca audsrv

Decodificar Vorbis a 48 kHz estereo cuesta ~34% del EE, asi que no cabe en el
bucle de dibujo. Va en su hilo, a prioridad `0x40` frente a `0x50` del principal
(la del principal se fija a mano con `ChangeThreadPriority`: no se puede dar por
hecho con cual arranca, y si quedara por encima, `graph_wait_vsync` dejaria la
musica sin CPU).

Comprobado desensamblando el SDK, no suponiendo:

- `padRead` = `SyncDCache` + `memcpy`, **sin SIF RPC**. El hilo de dibujo nunca
  toca el SIF.
- `libaudsrv` usa **un unico** `SifRpcClientData_t cd0`, protegido con
  `WaitSema`/`SignalSema`. Es reentrante, pero `audsrv_wait_audio` **retiene el
  semaforo mientras bloquea**: si el bucle de dibujo llamara a
  `audsrv_ch_play_adpcm`, se quedaria parado ahi decenas de ms.
- `sceSifCallRpc` en modo bloqueante duerme con `WaitSema`, no gira en vacio, asi
  que el hilo de audio cede la CPU de verdad.

De ahi la regla: **el hilo de audio es el unico que llama a audsrv**. El bucle de
dibujo, cuando quiere un sonido de golpe, solo incrementa `sfx_pedido[i]`. Sin
candados y sin nadie bloqueado. El precio es que el golpe llega con la latencia de
un trozo, por eso los trozos son de 2048 bytes (~10 ms) y no de 8192 (~42 ms).

## Resultados medidos

PCSX2 2.6.3, recompilador EE activo, `click140.ogg` (120 s, 48 kHz estereo) leido
del pen. 118 muestras, una por segundo, sobre la cancion entera:

| metrica | min | max | que significa |
|---|---|---|---|
| `dif_ciclos` | −128 | −127 | reloj de audio contra ciclos del EE: **pendiente 1.000, sin deriva en 2 min** |
| `dif_bytes` | 87 | 95 | profundidad de la cola de audsrv: acotada, no crece |
| `err_interp` | 6 | 8 | error del interpolador, arranque incluido |

**Sin tocar el mando:** 0 perfectos, 0 buenos, **275 fallos de 275**, 0 lecturas de
mando fallidas. Es el resultado correcto, y es la comprobacion de que no hay
golpes fantasma.

**Autoprueba** (`AUTOGOLPE 1`, `PULSO_INICIAL 1`, o sea notas ya en el segundo
0,4): **278 perfectos, 0 buenos, 0 fallos de 278**, y `dif_bytes` sin salirse de
87-95 en toda la cancion. Esto es lo que prueba lo que el resto del diseno da por
hecho: que **el streaming de PCM y los canales ADPCM conviven** — 278 disparos de
`audsrv_ch_play_adpcm` desde el hilo de audio sin despeinar la cola del streaming.
Ni `Sounds/` (solo ADPCM) ni `ogg_player.c` (solo streaming) llegaban a probarlo.

Lo que importa aqui **no es** que el reloj arranque en cero ni que una nota caiga
bonita encima de un clic: eso es de PCSX2 y no se traslada a la consola. Lo que se
traslada es la **pendiente**, y esa es 1.000 exacta.

## Partituras .tja

`tja.c` lee las partituras de open-taiko. De momento saca solo las notas simples
(1 don, 2 ka, 3 DON, 4 KA); rodillos y globos (5,6,7,8) se cuentan y se saltan.
Las grandes ya vienen distinguidas en `tja_nota_t.grande`, aunque el juego
todavia las trata como normales.

```c
#define RUTA_TJA  "mass0:/splice.tja"
#define CURSO     "Easy"      // o Normal / Hard / Oni / Edit, o "0".."4"
```

Si no aparece el `.tja`, se cae a la chart generada a mano sobre el `.ogg`
empotrado, asi que el motor sigue arrancando sin pen.

### Lo que hay que llevar bien del formato

La partitura son lineas de digitos donde cada digito es una subdivision del
compas y **la coma cierra el compas**. Cuantas subdivisiones tiene un compas lo
decide cuantos digitos le hayas puesto; no hay numero fijo. De ahi salen tres
cosas que no se pueden simplificar:

1. Un compas dura `(60000/bpm) * 4 * num/den`, con `num/den` del ultimo
   `#MEASURE`. `splice.tja` usa 4/4, 7/4, 7/8 y 8/4, y el primer `#MEASURE` esta
   en la linea siguiente a `#START`: si se ignora, la cancion entera se desplaza
   desde la nota uno.
2. `#BPMCHANGE` cambia el tempo a mitad de partitura (4 veces por curso en
   `splice.tja`). El tiempo hay que acumularlo compas a compas con el BPM
   vigente; no vale multiplicar.
3. **Un compas puede cruzar varias lineas**, y entre medias puede haber lineas de
   comando. En `splice.tja` hay un `100100` sin coma seguido de un `#GOGOSTART` y
   el compas sigue despues. Por eso el buffer se acumula entre lineas y solo lo
   cierra la coma.

Ademas: el fichero viene en **UTF-8 con BOM** (si no se salta, la primera clave
no se reconoce), y los comentarios `//` llegan al final de linea.

Si un `#BPMCHANGE` o `#MEASURE` cayera a mitad de compas, las notas de antes y de
despues durarian distinto y esto lo calcularia mal. En los 5 cursos de
`splice.tja` no pasa ni una vez, asi que se aplica el comando y punto — pero se
cuenta en `avisos_tiempo` para que no pase desapercibido con otro fichero.

### Como se verifico

Tres comprobaciones, de menos a mas independiente:

1. **Contra una implementacion en Python** escrita aparte desde la definicion del
   formato. Con `VOLCAR_CHART 1` la PS2 escupe el instante y el tipo de cada
   nota. Las **189 notas del curso Easy coinciden exactamente**, tiempo y tipo.
2. **Contra la musica.** Correlacion cruzada del tren de notas contra el flujo
   espectral de `splice.ogg`, barriendo desfases. Pico unico y estrecho, 16 veces
   sobre el fondo. Las hipotesis alternativas se quedan en el ruido:

   | hipotesis | correlacion |
   |---|---|
   | signo del OFFSET correcto | **pico** |
   | signo invertido (+328 ms) | 51 |
   | signo invertido (−328 ms) | 572 |
   | OFFSET ignorado (−164 ms) | 68 |

3. **Calibrando el propio detector.** El pico salia en −25 ms, que podia ser
   error de la chart o latencia de la ventana STFT. Pasando el mismo detector por
   `click140.ogg`, cuyos onsets conozco al milisegundo, el sesgo resulta ser
   −30 ms. Asi que **el error real de la chart es +5 ms**, por debajo de la
   resolucion de la medida.

Ojo con el metodo: la correlacion tiene picos anchos en multiplos del pulso (600
ms son 3 pulsos exactos a 300 BPM), asi que hay que barrer menos de medio pulso o
mirar solo el pico estrecho.

## El bucle de dibujo gira en vacio: nada puede esperar activamente

`graph_wait_vsync` y `draw_wait_finish` de ps2sdk **no duermen**: son bucles de
sondeo sobre registros del GS (desensamblados: `beqz v0, <atras>`). El hilo
principal nunca cede la CPU por su cuenta.

Consecuencia: **cualquier espera activa en el hilo de audio bloquea el programa**,
se ponga la prioridad que se ponga.

| prioridad del hilo de audio | que pasa |
|---|---|
| por encima de main | el audio gira y deja al juego sin CPU |
| por debajo de main | main gira y deja al audio sin CPU: `audio_terminado` no se pone nunca y la partida no termina |

Las dos versiones se colgaron de verdad al acabarse la cancion, y la segunda de
forma perfectamente reproducible. La solucion no es elegir prioridad: es **no
esperar**. Al terminar de decodificar, el hilo pone `audio_terminado` y sale. Lo
que queda en la cola lo toca el IOP solo, y el reloj sigue avanzando con
`cpu_ticks` desde la ultima marca publicada.

La regla para lo que venga: en este motor, **si un hilo tiene que esperar, tiene
que dormir de verdad** (semaforo o una llamada que bloquee, como
`audsrv_wait_audio`), nunca girar.

## Modo calibracion (paso 2)

`OFFSET_LATENCIA_MS` es lo que se le suma al reloj antes de que el juego lo use.
Existe porque entre "el motor cree que la cancion va por el segundo 12" y "tu oyes
el segundo 12" hay retrasos que el motor no puede ver:

- **audio**: `audsrv_queued()` cuenta su cola, pero no el DMA al SPU2, ni el DAC,
  ni el amplificador ni el altavoz.
- **video**: dibujas el frame N y se ve un vsync despues, mas lo que tarde la tele
  (en un LCD moderno, 10-100 ms).
- **entrada**: pulsas y no se entera hasta el siguiente `padRead`, hasta 20 ms.

Los tres empujan en el mismo sentido: hacen que quien golpea **perfectamente con
lo que oye** parezca llegar tarde. El offset cancela ese sesgo.

**No lo confundas con el `OFFSET:` del `.tja`.** Ese dice cuanto se desplaza la
partitura respecto a su fichero de audio, es de la cancion y viene dado. Este es
del equipo. Son dos sumandos distintos.

Para medirlo **ya no hay que recompilar**: se elige la pista `Metronomo
(calibrar)` en el menu, se juega, y la pantalla de resultados enseña la medida
y ofrece guardarla. Un golpe en el parche **rojo** la guarda en el pen.

```
=== CALIBRACION: 275 golpes ===
  p25 -2 ms | mediana 2 ms | p75 7 ms | dispersion 9 ms
  ==> offset 2 ms
```

La medida **solo se enseña en el metronomo**. En una cancion normal se sigue
apuntando (sale por consola al terminar), pero enseñarla en esos resultados era
ruido: un numero que ademas no se puede guardar desde ahi, encima de unos
resultados que no tienen nada que ver con el.

Detalles:

- Es **mediana**, no media: un despiste suelto arrastra la media y no mueve la
  mediana.
- La cuantizacion de 20 ms de los 50 Hz **se promedia sola** sobre muchos golpes,
  porque la fase entre tiempos de nota y tiempos de frame va rotando (es el
  patron de 12 s que salio en las pruebas). Por eso hacen falta decenas de golpes
  y no vale fiarse de uno.
- **El suelo de medida es ~9 ms de dispersion.** Con `AUTOGOLPE 1`, o sea golpes
  sinteticos sin latencia ninguna, sale `mediana 2, dispersion 9`. Si tu
  dispersion se acerca a 9 estas en el limite del montaje; si pasa de 40 el
  numero no es de fiar y avisa.
- Se apunta contra el reloj que **ya lleva el offset puesto**, asi que se puede
  recalibrar encima de un valor anterior sin hacer cuentas.
- **Solo el metronomo ensancha la ventana** de busqueda a 200 ms
  (`VENTANA_CAPTURA_MS`): con la normal de 108 ms, alguien 150 ms desviado no
  engancharia ni una nota y no habria nada que medir. En una cancion normal la
  ventana se queda en 108 y la medida sale sesgada hacia cero, porque los golpes
  muy desviados no llegan a contarse. Por eso la medida se **enseña** en
  cualquier cancion pero solo se **guarda** desde el metronomo.
- Guardar pide ademas que la pantalla de resultados lleve **medio segundo**
  puesta. Sin eso, quien sigue aporreando cuando acaba la cancion guardaria la
  calibracion sin querer.

### El menu de opciones

START desde la lista de canciones abre un panel en el centro:

```
OPCIONES
> Musica: ||||||||||
  Sonido: ||||||||**
  Borrar perfil (y puntuaciones)
  Metronomo (calibrar)
  Nivel de prueba
  Volver al selector
```

Aqui aparece el unico sitio donde los dos parches rojos **no** hacen lo mismo:
mover una barra pide dos sentidos y el tambor no tiene mas botones, asi que en
las filas de volumen el parche **izquierdo baja** y el **derecho sube**. En las
demas filas, cualquier rojo elige. La linea de ayuda de abajo cambia con la
fila, para que no haya que acordarse.

Al mover **Sonido** suena un golpe. No es un adorno: en el selector no hay
musica de fondo, asi que es la unica forma de oir lo que estas moviendo.

`audsrv_ch_play_adpcm` y `audsrv_set_volume` se llaman aqui **desde el bucle de
dibujo**, saltandose la regla de que solo el hilo de audio toca audsrv. Se puede
porque las opciones estan dentro del selector, y ahi ese hilo no existe.

El fichero se escribe **una vez, al cerrar**, y no en cada rayita: escribir en
el pen cuesta, y no hace falta hacerlo diez veces mientras alguien sube el
volumen.

**Borrar perfil pregunta antes**, y el cursor empieza sobre el NO. Borra con
`remove()` y, si el fichero sigue ahi, lo reintenta con `fileXioRemove()`;
despues comprueba abriendolo otra vez. No esta comprobado cual de las dos capas
borra de verdad en este ps2sdk, y un borrado que dice que si y deja el fichero
seria peor que uno que falla: al reiniciar reapareceria el perfil viejo sin
explicacion.

**Metronomo** y **Nivel de prueba** cierran el menu y entran directos a esa
pista, sin buscarla en la lista.

### Las puntuaciones

**Un `PUNTOS.CFG` dentro de la carpeta de cada cancion**, con la misma forma que
el perfil, una clave por linea y una linea por curso:

```
facil=12345
normal=23456
oni=45678
```

Asi la puntuacion viaja con la cancion (copias la carpeta a otro pen y se copia
con ella), no hay un indice central que se desincronice si alguien borra una
carpeta desde el PC, y el lector es el mismo `leer_clave()` del perfil.

`guardar_puntos()` **lee el fichero antes de escribirlo**: se reescribe entero,
asi que sin leer primero se perderian las marcas de los otros cursos.

Dos cosas del *cuando*:

- Se guarda tanto si la cancion acaba sola como si se corta con **TERMINAR Y
  VER RESULTADOS**. Hubo un rato en que solo se guardaba la que acababa sola,
  para que una partida a medias no dejara una marca floja; era una mala idea,
  porque una partida a medias suma menos puntos y no le gana a una entera de
  todas formas, y lo unico que conseguia era tirar la puntuacion de quien se
  sale a falta de diez segundos. **VOLVER AL MENU** sigue sin guardar: esa
  opcion ni llega a la pantalla de resultados.
- Se escribe **despues** de `detener_audio()`, nunca antes: escribir en el pen
  pasa por el SIF, y hacerlo con el hilo de audio alimentando a audsrv es pedir
  un corte de sonido.

La marca anterior se lee en `main`, antes de arrancar el hilo de audio: leer del
pen con la cancion ya sonando se nota.

En la pantalla de dificultad, debajo del nombre de la cancion, sale la marca del
curso que este marcado, y cambia al moverse por la fila. Las cinco se leen de
**una sola pasada** (`leer_puntos_todos`) y **una sola vez**, al entrar en esa
pantalla: cinco `leer_puntos()` abririan el fichero cinco veces, y en el pen
abrir cuesta mucho mas que leer. El cache no hace falta invalidarlo a mano
porque `menu()` se llama entera en cada vuelta del ciclo, asi que despues de
jugar vuelve vacio y la marca nueva se relee sola.

### El contador de calibracion

Jugando el metronomo, debajo del titulo sale `Calibracion 7 / 20`. El metronomo
dura dos minutos y con veinte golpes enganchados ya esta medido: sin el
contador no hay forma de saber cuando se puede cortar con la pausa.

### La primera vez que se enciende

Si al arrancar **no hay fichero de ajustes en el pen**, sale una pantalla que
ofrece calibrar antes de nada:

- **CALIBRAR AHORA** manda derecho al metronomo, sin pasar por el menu. El
  fichero lo escribe la pantalla de resultados cuando el jugador acepta la
  medida; si se va sin guardar, la proxima vez se le vuelve a preguntar, que es
  lo correcto: sigue sin estar calibrado.
- **JUGAR SIN CALIBRAR** escribe el fichero igualmente, con el offset de
  fabrica. Si no lo escribiera, esta pantalla saldria en **cada** arranque a
  quien ya ha dicho que no quiere calibrar.

Un fichero corrupto cuenta como que no hay nada: el offset que se estaria usando
seria el de fabrica y el jugador no tiene por que enterarse por su cuenta.

### Donde se guarda

En `mass0:/TATSUJI.CFG`, un fichero de texto con una clave por linea:

```
offset=-12
musica=10
sonido=8
```

Las claves que no esten se quedan con su valor por defecto, asi que un fichero
viejo (de cuando solo se guardaba el offset) sigue valiendo. El **offset manda**:
si esa clave falta o no se entiende, el fichero no cuenta como perfil y se
vuelve a preguntar al arrancar. Por eso se escribe la primera, que es la que
sobrevive si algun dia el fichero creciera mas de lo que se lee de una vez.

Texto y no binario a proposito: son veinte bytes, se puede abrir en el PC con
cualquier editor para ver que hay o arreglarlo a mano, y no hay que pensar en el
orden de los bytes. Cuando haya que guardar mas cosas (volumen, personalizacion)
se le añaden lineas, y el que lee se salta las que no conozca.

Va al **pen** y no a la memory card, y es una decision, no una comodidad: asi
**los datos del jugador viajan con las canciones**. Llevas el pen a otra consola
y llevas tu calibracion contigo. La tarjeta ademas necesitaria `mcman`/`mcserv`
y un camino nuevo entero.

Por el mismo motivo, las **puntuaciones** iran en un fichero dentro de la
carpeta de cada cancion, no todas juntas en la raiz: la puntuacion es de la
cancion, y si copias la carpeta a otro pen se copia con ella.

**`guardar_config()` relee lo que acaba de escribir** antes de decir que ha ido
bien. No es paranoia: no esta comprobado que el `bdmfs_fatfs` de este ps2sdk
escriba de verdad, y un `fopen("wb")` que devuelve un `FILE*` valido y luego no
deja nada en el pen daria un "guardado" mentiroso que solo se descubriria al
reiniciar la consola. Lo que salga se enseña en pantalla, tanto si va bien como
si no. **Si el pen resultara ser de solo lectura, el plan B es la memory card.**

### Volver a pasar las pruebas

```c
#define AUTOGOLPE      1   // golpea solo en el instante de cada nota
#define LOG_FRAMES     1   // saca fps y peor hueco entre frames
#define PULSO_INICIAL  1   // mete notas en el primer segundo
```

(El informe de desfase ya no tiene interruptor: sale siempre al terminar.)

Con `AUTOGOLPE 1` + `PULSO_INICIAL 1`: **278 perfectos, 0 buenos, 0 fallos**.
Con todo a 0 y sin tocar el mando: **275 fallos de 275**.

## El ciclo menu -> partida -> menu

Hasta aqui `main()` cargaba una cancion, entraba en `render()` y se acababa.
Ahora es un ciclo:

```
menu()            elige cancion y dificultad, devuelve al pulsar CRUZ
pantalla_cargando()  un fotograma antes de bloquearse leyendo el pen
cargar_audio_de()    el .ogg, con cache de un hueco
preparar_cancion()   TODO el estado compartido a cero
lanzar_hilo_audio()
render()          partida y pantalla de resultados, vuelve con CRUZ
detener_audio()   corta el hilo y vacia la cola del IOP
```

`audsrv_init()`, `audsrv_adpcm_init()` y las tres muestras ADPCM se quedan
donde estaban, en el arranque: **entre canciones no se toca audsrv**, solo se
rehace el hilo. `audsrv_quit()` es unicamente para salir del programa.

### Lo que hay que poner a cero, y por que va todo junto

Cada linea de `preparar_cancion()` corresponde a una forma distinta de que
la SEGUNDA cancion salga mal, y ninguna se parece a un fallo de audio. Por
eso estan juntas en una funcion en vez de repartidas por `main()`:

| Si no se pone a cero | Lo que se ve en la vuelta 2 |
|---|---|
| `notas[i].resuelta` | ni una nota en pantalla y la cancion "termina" al instante |
| `audio_terminado` | se va derecho a la pantalla de resultados |
| `duracion_ms` | un audio que no arranca pasa por bueno (se comprueba `terminado && duracion == 0`) |
| `reloj_estable`, la base del seqlock | el reloj arranca torcido y el registro de deriva sale absurdo |
| `sfx_servido[]` | un golpe fantasma en el primer frame |

### Dos detalles que no son evidentes

**El hilo se borra a si mismo.** `hilo_audio` acaba en `ExitDeleteThread()`,
no en `ExitThread()`. Se crea un hilo por cancion, y `ExitThread` deja el TCB
ocupado: con unas cuantas partidas seguidas se acabarian los huecos y
`CreateThread` empezaria a fallar. Y no vale que `main` haga `DeleteThread`
mirando `audio_terminado`, porque esa bandera se pone **antes** de salir:
borraria un hilo todavia en ejecucion.

**El audio se corta en seco al acabar la cancion**, no al salir de la
pantalla de resultados. `hilo_audio` deja a proposito lo que quede en la cola
del IOP sonando (ver mas arriba: es lo que mantiene el reloj avanzando al
final de la cancion), pero cuando esa cola se vacia audsrv **no se calla: se
queda repitiendo el ultimo trozo**. Eso era la nota constante que sonaba por
debajo de los resultados hasta volver al menu.

Asi que `render()` llama a `detener_audio()` en el mismo momento en que pone
`fin = 1`. Cuesta la espera de un trozo (~10 ms) y es ademas justo lo que va a
necesitar el menu de pausa: callar el audio en el frame en que se para el
juego. `detener_audio()` es idempotente porque `main` la vuelve a llamar al
cerrar el ciclo.

### El menu se desplaza, no crece

Solo se pintan `FILAS_MENU` (10) canciones a la vez, con el cursor centrado
mientras se pueda. Por dos motivos, y el segundo no se ve venir: 10 filas de
24 px son 240 de los 430 utiles de la zona segura, **y** a ~8 qwords por
caracter una lista de 32 titulos de 30 letras se iria a ~7700 qwords de los
8192 del packet. Un desbordamiento ahi no da error: pinta basura.

### El boton que se quedaba pulsado

`prev_btns` arranca en `0x0000` y no en `0xFFFF`, tanto en `menu()` como en
`render()`. En libpad la logica va invertida (1 = suelto), asi que empezar
con todo a cero significa **"todo pulsado"**: ningun boton cuenta hasta que
se suelta y se vuelve a pulsar.

Sin esto, la CRUZ con la que se sale de la pantalla de resultados seguia
pulsada al llegar al menu y el menu volvia a entrar en la cancion en el
primer frame, sin dar tiempo ni a verlo.

### La negrita va fija

`fontx_load(..., 1)`. PAL 640x512 es **entrelazado** y los trazos de KROM
tienen un pixel de grosor: caen en un solo campo y tiemblan. Hubo un rato un
conmutador con el TRIANGULO para poder decidirlo mirandolo; se miro, la
negrita gana de calle, y el conmutador sobra.

### START en el menu no hace nada

Y es a proposito. Antes salia del bucle de `main`, que cerraba audsrv y se
iba a `SleepThread()`. En pantalla eso es **exactamente** un cuelgue: el
ultimo fotograma clavado y nada que responda. Una PS2 no tiene a donde
"salir": se apaga o se resetea. `menu()` devuelve `void` justamente para que
no haya forma de volver a escribir ese camino.

### Cache del .ogg, y por que la RAM no crece

Repetir cancion no vuelve a leer del pen. En la consola son 4 MB por el bus
USB 1.1 (~1 MB/s): varios segundos cada vez.

El hueco es **uno solo**, y `cargar_audio_de()` libera el anterior **antes**
de pedir el siguiente. O sea que encadenar canciones no va acumulando: hay un
`.ogg` vivo a la vez, se llamen 4 o 40. La chart tampoco: `tja_chart_t` es un
`static` de ~50 KB que se reusa, y los bytes del `.tja` se liberan nada mas
parsear.

Lo que si podria pasar con bloques de 4 MB es que el monton se **fragmente**:
`free()` de newlib no le devuelve nada al sistema, y alternar entre un `.ogg`
de 4 MB y otro de 6 podria dejar huecos inservibles. Por eso el menu enseña
`RAM: N KB usados  N KB libres` (`mallinfo()`): si "libres" va bajando
cancion tras cancion, es eso. Encadenando canciones en el emulador se queda
plano.

## La pausa

START durante la cancion la abre. Tres opciones:

| | |
|---|---|
| **REANUDAR** | cuenta atras de 3 segundos y sigue por donde iba |
| **TERMINAR Y VER RESULTADOS** | corta la cancion y saca el resumen |
| **VOLVER AL MENU** | lo deja, sin resumen |

**Tres y no dos.** "Terminar" y "volver al menu" parecen lo mismo y no lo son:
terminar saca el resumen, y eso es lo unico que permite calibrar sin tragarse
los dos minutos enteros del metronomo. Juntarlas obligaria a que una de las dos
hiciera algo distinto de lo que dice su nombre.

La cuenta atras va **antes** de despertar al hilo de audio, no despues: asi la
musica arranca justo cuando se acaba la cuenta, y con el reloj todavia congelado
el juego no avanza ni un milisegundo mientras tanto. Se mide con `cpu_ticks` y
no contando fotogramas, porque un fotograma dura 20 ms en PAL y 16,7 en NTSC:
contando frames, la misma cuenta duraria tres segundos aqui y dos y medio en
otra consola.

La ejecuta **el hilo de audio**, no el bucle de dibujo, porque es el unico que
puede tocar audsrv mientras la cancion suena. El bucle de dibujo solo la pide y
espera el acuse:

```
dibujo:  pausa_pedida = 1  ->  espera pausa_activa
audio:   audsrv_stop_audio(); congelar_reloj(); pausa_activa = 1; SleepThread()
dibujo:  ...menu de pausa...
dibujo:  pausa_pedida = 0; WakeupThread()  ->  espera !pausa_activa
audio:   set_format; reloj otra vez; pausa_activa = 0
```

### Vaciar la cola no es limpieza, es obligatorio

Si solo se dejara de mandar PCM, la cola del IOP seguiria sonando unos cientos
de milisegundos y, al vaciarse, **audsrv no se calla: repite el ultimo trozo**.
Es la nota clavada que ya salio al acabar una cancion.

### Y por eso no hace falta buscar dentro del `.ogg`

Al vaciar la cola, la posicion que suena pasa a ser exactamente el byte que toca
decodificar: `enviados / bps`. Es justo lo que calcula el reloj cuando
`audsrv_queued()` da cero. Asi que reanudar es **seguir decodificando**, sin
`ov_pcm_seek` ni nada parecido. Lo unico que se pierde son los ~100 ms que
quedaban en la cola, que se oyen como un salto minusculo hacia delante.

### El reloj tiene que congelarse DENTRO del lector

`leer_reloj_bruto_ms()` interpola con `cpu_ticks()` desde la ultima marca
publicada. Sin congelarlo, una pausa de treinta segundos volveria con el reloj
**treinta segundos por delante**, y un solo fotograma con ese valor manda al
saco de fallos todas las notas que quedaban. Se congela dentro del lector y no
en el bucle de juego a proposito: asi protege a cualquiera que lea el reloj y no
solo al sitio que uno se acuerde de tapar.

Y al reanudar, `descongelar_reloj()` publica **antes** de descongelar, para que
nadie llegue a leer la marca vieja con el reloj ya corriendo.

### `SleepThread`, no una espera en vacio

Ya se sabe de antes que el bucle de dibujo gira sin dormir. Un hilo de audio con
mas prioridad girando en la pausa dejaria al juego sin CPU y la pausa se veria
igual que un cuelgue. `SleepThread`/`WakeupThread` duermen de verdad, y
`WakeupThread` lleva cuenta, asi que si llegara antes de que el hilo se durmiera
no se perderia.

### Las tres formas en que esto se cuelga

Las tres estan tapadas, y ninguna es teorica:

1. **Pausar sin hilo de audio.** Si la chart dura mas que su `.ogg` (el motor
   ya lo avisa por consola), el hilo termino hace rato y no hay nadie que
   conteste al acuse. La espera mira `audio_terminado` ademas de `pausa_activa`,
   y si no hay hilo se para el reloj desde el propio bucle de dibujo ("pausa en
   frio").
2. **Volver al menu desde la pausa.** El hilo esta dormido: `detener_audio()`
   pone `pausa_pedida = 0` y lo **despierta** antes de esperarlo, y lo primero
   que hace el hilo al despertar es mirar `parar_audio`. Sin eso, la espera de
   `detener_audio` se comeria sus tres segundos y el hilo se quedaria vivo con
   audsrv cogido.
3. **La cancion siguiente.** `preparar_cancion()` pone a cero `pausa_pedida`,
   `pausa_activa`, `hilo_audio_id` y `reloj_congelado`. Si no, el hilo de la
   cancion siguiente se dormiria en su primera vuelta: sin musica y sin reloj,
   o sea un cuelgue.

Al reanudar se vuelven a fijar formato y volumen. No deberia hacer falta, pero
**no esta comprobado** que audsrv acepte PCM otra vez despues de un
`stop_audio`, y si no lo aceptara la cancion volveria muda de la pausa sin decir
nada.

## Como se puntua, y de donde salen los numeros

Todo lo de esta seccion sale del codigo de OpenTaiko, no de foros. Se cita el
fichero para poder volver a mirarlo.

### Las ventanas de juicio son tres, no dos

`OpenTaiko/src/Common/CConfigIni.cs`, tabla `tzLevels`, en milisegundos:

| nivel | 良 perfecto | 可 bueno | 不可 | cuando se usa |
|---|---|---|---|---|
| Lv0 | 75 | 108 | 125 | Facil/Normal + mod "Loose" |
| Lv1 | 58 | 108 | 125 | Facil/Normal + "Lenient" |
| **Lv2** | **42** | **108** | **125** | **Facil/Normal, por defecto** |
| Lv3 | 42 | 75 | 108 | Dificil+ con "Lenient" |
| **Lv4** | **25** | **75** | **108** | **Dificil/Oni/Edit, por defecto** |
| Lv5 | 25 | 58 | 108 | Dificil+ con "Strict" |
| Lv6 | 17 | 42 | 108 | Dificil+ con "Rigorous" |

El reparto lo decide `tEasyTimeZones()`: **dificultad <= Normal usa la tabla
facil**. Aqui es `curso <= 1`.

Dos cosas que estaban mal y ahora no:

1. **Se usaba 25/75/108 para todo**, o sea las ventanas de Oni tambien en
   Facil. Ahora Facil y Normal van con 42/108/125.
2. **Faltaba la tercera zona.** Un golpe a 90 ms daba "bueno"; en el juego de
   verdad eso es 不可: se come la nota **y rompe el combo**. Mas alla de la
   tercera zona el golpe no engancha nada y no pasa nada.

Y el globo abre su ventana **17 ms antes** de la cabeza
(`evaluateNodeJudge`: `pChip.n発声時刻ms - 17`). Los rodillos no llevan ese
adelanto. Ojo al leer ese codigo: `ENoteJudge.Perfect` es 良 y
`ENoteJudge.Great` es 可, estan cruzados respecto a lo que sugieren los nombres.

### El reparto shin'uchi

La partitura **entera** vale un millon:

```
puntos_por_nota = ceil( (1000000 - globos*100 - 16,6*seg_rodillo*100) / notas / 10 ) * 10
```

- Un **bueno** vale la mitad, redondeada a decenas: `nota / 20 * 10`
- **Rodillo** y **globo**: 100 por golpe
- Las notas **grandes NO valen doble**, y el **Gogo Time NO multiplica**
- Reventar un globo **no da premio** (en el sistema viejo son 5000)

El `16,6 * segundos` es cuantos golpes por segundo da por hecho el juego que se
pueden dar en un rodillo: reserva ese trozo del millon para los rodillos y
reparte el resto entre las notas.

**Por que este y no el viejo** (`SCOREINIT`/`SCOREDIFF`, donde la nota sube de
valor con el combo y las grandes valen doble): por tres motivos que son de este
proyecto y no de gustos.

1. No necesita **nada** del `.tja`. Los tres ficheros de prueba no traen
   `SCOREINIT` ni `SCOREDIFF`, que es lo normal. Sin ellos, OpenTaiko tira de
   300 y 120 por defecto, que son numeros sacados de la nada.
2. No necesita **Gogo Time**, que no parseamos.
3. Da la **misma escala 0..1000000 para todas las canciones**, que es lo unico
   que hace comparables las puntuaciones que se guardan.

### Comprobado contra partituras de verdad

`prueba_tja.c` lleva una copia de la formula (hay que tocar las dos juntas:
`motor.c` no se puede compilar en el PC). Una vuelta perfecta de los 14 cursos
de las tres canciones de prueba:

```
After Epochs Easy   5620 x 175 notas + 10855 ms rodillo = 1001519
After Epochs Oni     950 x 1049 + 18 globos + 3553 ms   = 1004247
Cubibibibism Hard   1220 x 824 + 12 globos + 649 ms     = 1007557
splice Edit          630 x 1603                         = 1009890
```

Todos entre 1 000 244 y 1 009 890: el redondeo hacia arriba siempre pasa un
poco del millon, que es lo que hace el original.

**Lo que aqui no cuadra con una consola**: el reparto da por hecho 16,6 golpes
por segundo en los rodillos, que es lo que se puede aporrear en un tambor de
verdad. Con mando se pueden sacar dos flancos por fotograma (100/s a 50 Hz), y
entonces un rodillo largo da tres veces lo que tenia reservado. OpenTaiko
tampoco lo limita.

## El alma (魂ゲージ)

De OpenTaiko: `CAct演奏ゲージ共通.cs` para el llenado, `HGaugeMethods.cs` para la
norma.

```
tasa   = 70,7 si LEVEL <= 7 | 70,0 si LEVEL = 8 | 75,0 si LEVEL >= 9
daño   = 0,625 si LEVEL <= 8 | 2,0 si LEVEL >= 9

subida_por_nota = 10000 / (notas * tasa)      <- en puntos de %

  perfecto -> +subida      bueno -> +subida/2      fallo -> -subida*daño
```

La `tasa` es literalmente **a que porcentaje de las notas se llena el alma
entera**: con 70,7, clavando el 70,7 % de las notas ya estas a 100.

**Empieza en cero, se recorta a [0, 100] y solo se mira AL FINAL.** En taiko no
se muere a mitad de cancion: se aprueba o no se aprueba. (Las barras que matan
—`HARD`, `EXTREME`— son mods, empiezan a 100 y van bajando; no estan aqui.)

**Rodillos y globos no la tocan**, igual que en la puntuacion: solo cuentan las
notas que se pueden fallar.

**Norma para aprobar**: Facil 60, Normal 70, Dificil 70, Oni/Edit 80. Pero el
**LEVEL manda sobre la dificultad** a partir de 11: nivel 11 -> 88, 12 -> 92,
13+ -> 96. Un Oni del 11 pide 88, no 80.

Con las canciones de prueba:

| | notas | tasa | +bueno | −fallo | norma | notas clavadas para aprobar |
|---|---|---|---|---|---|---|
| After Epochs Facil | 190 | 70,7 | +0,744 | −0,465 | 60 | 81 (42 %) |
| After Epochs Oni | 1049 | 70,0 | +0,136 | −0,085 | 80 | 587 (56 %) |
| Cubibibibism Oni | 1371 | 75,0 | +0,097 | −0,195 | 80 | 823 (60 %) |
| splice Edit | 1603 | 75,0 | +0,083 | −0,166 | 88 | 1058 (66 %) |

Mira el escalon del `daño`: en After Epochs Oni (nivel 8) un fallo cuesta 0,6
notas; en Cubibibibism Oni (nivel 10) cuesta **2 notas**. Eso es lo que hace que
los niveles altos castiguen de verdad.

La barra se dibuja **con las figuras y no con el HUD**, por la regla de
siempre: el texto estampa la Z maxima y cualquier figura mandada despues se
perderia. Delante lleva el 魂 (`0x8DAC` en Shift-JIS, crudo porque es
constante), a x=-180: el aviso del globo se pinta en `X_JUEZ - 30` = -280 y con
`GLOBO 12` llega hasta -216, asi que quedan 36 px de aire.

## Texto en japones

**Las fuentes japonesas estan en la BIOS de la consola.** `rom0:KROM` lleva las
dos mitades: la de un byte (ASCII / JIS X 0201) al final, en 0x198DE, y la de
dos bytes (kana, simbolos y kanji) al principio. `fontx_print_sjis()` de ps2sdk
mezcla las dos en la misma cadena el solo.

### El `fontx_load(DOUBLE_BYTE)` de ps2sdk NO funciona

Esto costo un arranque colgado, asi que queda escrito. En
`ee/font/src/fontx.c`, `fontx_load_double_krom()`:

```c
fontx_header->type      = DOUBLE_BYTE;      // en la struct, byte 18
fontx_header->table_num = table_num;        // en la struct, byte 19
memcpy(fontx->font + 18, sjis_table, 204);  // <- se lleva los dos por delante
```

La cabecera FONTX2 **del formato** ocupa 17 bytes (18 con el numero de tablas),
pero la **struct** de la libreria mete un byte de mas en el identificador y otro
en el nombre para los terminadores, asi que sus campos caen en 16, 17, 18 y 19.
La libreria escribe la tabla de rangos donde dice el formato (byte 18) y luego
la lee donde dice la struct (byte 20): dos bytes de desfase, y de paso el
`memcpy` pisa el `type`. Justo despues, `fontx_load` compara ese `type` con el
que le pediste, no cuadra, imprime "Type mismatch" y devuelve **-1**.

O sea que ese camino no puede funcionar para nadie, no es cosa de esta consola.

`cargar_krom_kanji()` monta el buffer a mano, tal y como lo **lee**
`fontx_get_char`, y lee los 102 KB de glifos a trozos de 8 KB. Comprobado con
la aritmetica de indices de la propia libreria: los 3489 caracteres de los 51
rangos dan indices 0..3488, todos distintos, que es exactamente lo que reserva
el buffer.

**Y si falla, NO se para.** Se sigue con el font ASCII a secas: los titulos
japoneses saldran ilegibles, pero el juego arranca. El `SleepThread()` que habia
ahi al principio convertia el fallo en una pantalla congelada nada mas arrancar,
que es justo lo que no se puede permitir. Todo
el texto del motor pasa ahora por ahi: para una cadena de solo ASCII hace
exactamente lo mismo que `fontx_print_ascii` (mismos margenes, mismo `x_orig` en
`LEFT_ALIGN`), asi que no hay dos caminos que mantener.

### Que trae KROM, y que no

3489 glifos, segun la tabla de rangos de `ee/font/src/fontx.c` del propio
ps2sdk: simbolos, hiragana, katakana, griego, cirilico y los **2965 kanji de
JIS nivel 1**. El **nivel 2 no esta** (kanji raros). El fallo es benigno:
`fontx_get_char` devuelve NULL, el caracter se salta y deja el hueco — no pinta
basura. Aun asi, la conversion los sustituye por `?`, que dice mas.

### La tabla

`gen_sjis.py` genera `sjis_tabla.c` **a partir de esa misma tabla de rangos**.
Eso es lo que garantiza que no haya ni una entrada para la que la consola no
tenga glifo: no se inventa nada, se pregunta a la fuente. Son 3489 entradas de
4 bytes = **13,6 KB** en el `.elf`, ordenadas por Unicode para buscar en
binario.

### Los .tja vienen en dos formatos

Los modernos en UTF-8 (los tres de prueba, con BOM); los antiguos en Shift-JIS.
`a_sjis()` valida la cadena como UTF-8: si lo es, convierte; si no, la copia tal
cual, porque entonces ya esta en el formato que quiere `fontx_print_sjis`. Una
cadena de solo ASCII es UTF-8 valido y se convierte en si misma, asi que este
unico camino vale para todo.

**Donde falla la deteccion**, que conviene saberlo: un kanji en Shift-JIS con
cabecera `0xE0-0xEF` y dos bytes de cola en `0x80-0xBF` forma una secuencia
UTF-8 valida y se leeria mal. Pero esas cabeceras son las del **nivel 2**, y
basta con que en toda la cadena haya UN caracter de cabecera `0x81-0x9F` (todo
el nivel 1, todo el kana) para que la validacion falle y se tome el camino
bueno. Para colarse haria falta un titulo en Shift-JIS hecho **solo** de kanji
del nivel 2 — que ademas KROM no sabe dibujar. Se acepta a cambio de no meter
heuristicas que puedan equivocarse en el caso normal.

La conversion se hace **una vez, al catalogar**, y el titulo se guarda ya
convertido. Hacerla al pintar seria repetirla cincuenta veces por segundo, y
hacerla mas tarde obligaria a acordarse en los seis sitios que pintan el
titulo.

### Recortar sin partir un kanji

`recorte_sjis()` y no `"%.28s"`: el printf corta por bytes y puede dejar el
primer byte de un kanji suelto, que `fontx_print_sjis` toma por cabecera y
entonces se come el terminador y sigue leyendo.

Recortar por **bytes** (y no por caracteres) es ademas lo que hace que las
medidas de siempre sigan valiendo: un kanji ocupa dos bytes y se dibuja al doble
de ancho que una letra, asi que el ancho en pixeles sale igual. La zona segura y
el presupuesto del packet no cambian.

### Comprobado en el PC

`sjis.c` es C portable a proposito, sin nada de PS2, para poder comprobarlo sin
consola:

```sh
gcc -O1 -Wall -Wextra -I. -o /tmp/prueba_sjis prueba_sjis.c sjis.c sjis_tabla.c
/tmp/prueba_sjis "../Canciones prueba"/*/*.tja
```

Saca los bytes convertidos en hexadecimal y el script que lo llama los vuelve a
descodificar con la tabla de Python para compararlos con el original. **Ida y
vuelta correcta en los 10 titulos y subtitulos** de las tres canciones,
japoneses incluidos. Casos limite comprobados a mano:

| entrada | sale |
|---|---|
| `splice` (ASCII) | igual |
| `きゅ` en UTF-8 | `82ab 82e3` |
| `きゅ` ya en Shift-JIS | igual, sin tocar |
| `齢` (JIS nivel 1) | `97ee` |
| `弌` (JIS nivel 2) | `?` |
| emoji (fuera del BMP) | `?` |
| `AきB` | `41 82ab 42` |
| recorte a 3 bytes de `きゅび` | `82ab` (no parte el segundo) |

### Como verlo funcionando

Las tres canciones de prueba traen el `TITLE` en latino y el japones en
`TITLEJA`, asi que **con ellas no se ve nada**. Por eso la pista de prueba se
llama `Prueba de notas (かな漢字)`: lleva kana y kanji a proposito, y sale desde
el menu de opciones. Para ver un titulo de cancion de verdad en japones basta
con cambiar `TITLE:` por el contenido de `TITLEJA:` en cualquier `.tja`.

## Los comandos del .tja

Lo que se hace con cada uno, y por que. Las tres canciones de prueba traen 448
`#SCROLL` y 59 `#GOGOSTART`, asi que no son casos raros.

| comando | que se hace |
|---|---|
| `#BPMCHANGE`, `#MEASURE` | cambian el tiempo. Ya estaban |
| `#SCROLL` | multiplica la distancia a la que se dibuja cada nota |
| `#GOGOSTART` / `#GOGOEND` | marca las notas; el circulo del juez se pone naranja |
| `#DELAY` | mete un silencio: todo lo que viene detras se retrasa |
| `#BRANCHSTART` / `#N` `#E` `#M` / `#BRANCHEND` | se lee **una** rama, las otras se saltan |
| `#BARLINEON/OFF`, `#JPOSSCROLL`, `#SUDDEN`, `#HBSCROLL`, `#NMSCROLL` | efectos de dibujo, se ignoran |

### `#SCROLL` va por NOTA, no por compas

Un `#SCROLL` puede caer en mitad de un compas y afectar solo a las notas que
van detras. Por eso el scroll (y el gogo) se guardan en un array paralelo al de
digitos del compas, uno por nota, en vez de en una variable suelta.

Los valores **complejos** (`0+1i`, `-0.4-0.8i`, 8 de ellos en Cubibibibism Edit)
son efectos de mover las notas en vertical o en diagonal, que aqui no se hacen.
Se ponen a velocidad **normal** y no a su parte real: un `0+1i` daria scroll 0 y
la nota se quedaria clavada en el juez desde el principio de la cancion. Los
valores se acotan ademas a ±20, porque hay charts con barbaridades.

### El corte del bucle de dibujo ya no puede ser por posicion

Antes bastaba con "esta nota se ha salido por la derecha, corta": con todas a la
misma velocidad, la de detras estaba aun mas lejos. Con `#SCROLL` **eso deja de
ser cierto** — una nota posterior con scroll bajo puede estar en pantalla
cuando la anterior todavia no ha entrado.

Ahora el corte es por **tiempo**, con una ventana que sale del `#SCROLL` mas
bajo de la chart: una nota se ve cuando su distancia baja de ~670 px, o sea a
los `670 / (PX_POR_MS * scroll)` ms de llegar. Los scroll diminutos (hay charts
con 0,003) se dejan fuera del calculo con un suelo de 0,2, o la ventana se iria
a minutos.

### Las bifurcaciones: una rama y las otras a la basura

`#BRANCHSTART` abre un tramo escrito **tres veces** — la version normal (`#N`),
la avanzada (`#E`) y la maestra (`#M`)— y el juego salta de una a otra segun
como lo estes haciendo. Aqui no se evalua la condicion: se lee la primera
seccion que aparezca y las otras se saltan enteras.

Saltarlas es obligatorio, no una comodidad: **las tres describen los mismos
compases**. Antes de esto, el parser no conocia esos comandos y se leia todo
seguido, asi que una cancion con bifurcaciones tenia el triple de notas y cada
tanda desplazada un tramo entero. Ninguna de las tres canciones de prueba las
usa, asi que este camino **no esta probado con una chart de verdad**.

## Los cuatro tipos de nota

| .tja | que es | como se juega |
|---|---|---|
| 1, 2 | don, ka | golpe en su ventana |
| 3, 4 | DON, KA grandes | igual, pero circulo mas gordo con aro y sonido grande |
| 5, 6 | rodillo (y rodillo grande) | barra amarilla: cada golpe dentro del tramo suma |
| 7 | globo | hay que darle N veces antes de que acabe el tramo |
| 8 | cierra el tramo abierto | — |

Los tramos (5, 6, 7) salen del parser como **una sola entrada** con
`tiempo_ms` de inicio y `fin_ms` de final, y las notas normales llevan
`fin_ms == tiempo_ms`. Asi el motor las trata a todas igual sin mirar el tipo
en cada sitio.

### Las grandes valen con un golpe, y eso NO esta terminado

En el taiko de verdad una nota grande pide los dos parches del mismo color a
la vez. De momento cuentan con un golpe y solo se distinguen por el tamaño, el
aro y el sonido.

Con el mapeo a cuatro botones ya **se puede** pedir (hay dos rojos y dos
azules), pero todavia no se pide: falta detectar los dos flancos dentro de una
ventana corta y dar bonus solo entonces. Ojo con como se haga: en el taiko de
verdad, darle con una sola mano a una nota grande **sigue contando** como
acierto, solo que sin el extra. Convertirlo en fallo seria mucho peor que
dejarlo como esta.

### Los tramos se comen el golpe ANTES de juzgar notas

El orden importa y no es el evidente. Si se juzgara primero la nota, un toque
dentro de un rodillo se comeria la nota que viene detras —`nota_mas_cercana`
busca en +-108 ms— y encima el rodillo no sumaria. Asi que primero se mira
`tramo_activo()`, y si hay tramo abierto el golpe se lo lleva el tramo y ya
no se juzga contra nada.

`nota_mas_cercana()` no necesita filtro extra: compara por tipo y los tramos
no son ni DON ni KA.

### La retirada mira `fin_ms`, no `tiempo_ms`

`siguiente_nota` avanza sobre entradas ya pasadas, y el bucle de dibujo
empieza ahi. Con `tiempo_ms`, un rodillo de dos segundos y medio se retiraria
—y dejaria de dibujarse— casi dos segundos antes de acabar. Con `fin_ms` sale
gratis, porque en una nota normal los dos valores son el mismo.

Un rodillo o un globo que se pasa **no es un fallo**: en taiko no rompen el
combo, simplemente no suman.

### Tramos mal formados

Charts reales traen `8` sueltos y tramos sin cerrar. El parser define los tres
casos y los cuenta en `avisos_rodillo`:

- **apertura con otra abierta** -> cierra la anterior ahi mismo. Anidar no
  significa nada en taiko, y dejarlo pasar dejaria la primera con `fin_ms`
  sin poner: una barra hasta el infinito y una ventana de golpe que se traga
  todo lo que quede de cancion.
- **`8` sin nada abierto** -> se ignora.
- **tramo vivo al llegar al `#END`** -> se cierra donde acabo la chart.

En las tres canciones de prueba, los 15 cursos emparejan perfecto: cero
avisos. Pero el modo de fallo que se evita aqui es silencioso, y por eso esta
escrito y contado en vez de dado por hecho.

### BALLOON: y los golpes de cada globo

La clave `BALLOON:` da los golpes de cada globo **en el orden en que salen en
la chart**. Puede venir en la cabecera (vale para todos los cursos) o dentro
de un curso (pisa a la de cabecera); en los ficheros de prueba siempre viene
dentro del curso.

Solo se hace caso a la del curso pedido o a la de la cabecera. La de **otro**
curso se ignora a proposito: pisaria la buena, y como los cursos se recorren
en orden de fichero, el resultado dependeria de cual estuviera antes.

Un globo sin cuenta se queda en `TJA_GOLPES_POR_DEFECTO` (5) y suma en
`globos_sin_cuenta`. Nunca 0: un globo de cero golpes se reventaria solo en el
primer frame.

### Comprobar el parser sin PS2

`tja.c` es C del todo portable, asi que se compila nativo y se le pasan los
`.tja` de verdad. Es mucho mas rapido que mirar tiempos de rodillo en el
emulador:

```sh
gcc -O1 -Wall -Wextra -I. -o /tmp/prueba_tja prueba_tja.c tja.c
/tmp/prueba_tja "../Canciones prueba"/*/*.tja
```

Saca, por curso, el recuento por tipo y el inicio, el final y los golpes de
cada tramo.

### La cancion "Prueba de notas"

Al final de la lista, junto al metronomo, hay una cancion sin fichero con la
chart hecha por codigo (`generar_chart_prueba`). Un grupo por cada cosa que
sabe hacer el motor, en orden y separados por silencios:

```
4 don -> 4 ka -> 8 alternando a medio pulso
2 DON grandes -> 2 KA grandes
rodillo corto -> rodillo largo -> rodillo GRANDE
globo de 5 golpes -> globo de 10
y una mezcla de todo
```

31 entradas en 32 segundos. **No es para jugar, es para ver**: si una nota
grande no se distingue, un rodillo no suma o un globo no revienta, se ve en
medio minuto y sin depender de que una cancion real traiga ese caso pronto.

Tira del `click140.ogg` empotrado igual que el metronomo. **Sin audio no hay
reloj** —sale de la cola de audsrv, ver arriba— asi que "una pista sin musica"
no es una opcion; y de paso los clics sirven de referencia de tiempo.

Con `-DAUTOCICLO=1 -DAUTOGOLPE=1 -DAUTOCICLO_CANCION=4` sale, y repetido:

```
Chart de prueba: 31 entradas, acaba en 31714 ms
Resultado: 24 perfectos, 0 buenos, 0 fallos (de 31)
  combo max 24, rodillos 257 golpes, globos 3 de 3
```

31 = 24 notas + 4 rodillos + 3 globos. Los 257 golpes de rodillo son los
5142 ms de tramo a 50 fps: el numero cuadra al golpe, que es lo que dice que
las ventanas estan donde tienen que estar.

## El recorrido del pen

`catalogar_canciones()` espera a que aparezca `mass0:/` y de ahi tira
`escanear_carpeta()`, que usa `fileXioDopen/Dread/Dclose`.

Valen las tres formas que se ven en la practica, porque cada carpeta cataloga
su `.tja` **y ademas** baja a las subcarpetas:

```
mass0:/algo.tja                 .tja suelto en la raiz
mass0:/Splice/splice.tja        una carpeta por cancion  <- lo normal
mass0:/J-POP/Splice/splice.tja  una capa de genero por encima
```

`PROF_MAX` corta a dos niveles. No es por elegancia: sin tope, un enlace
circular en el FAT dejaria el escaneo dando vueltas para siempre, y en una
consola eso es un cuelgue sin mensaje.

### Cortar en el primer .tja era un fallo

La primera version paraba de bajar en cuanto encontraba un `.tja`: "una
cancion por carpeta". Parece razonable y rompe el caso mas normal de todos —
con un `.tja` suelto en la raiz, la raiz entera contaba como cancion y no se
miraba ni una carpeta.

### Cual de los .ogg de la carpeta

El `.tja` lo dice en su clave `WAVE:`, pero **de ese nombre no te puedes
fiar**. Segun con que se copiara el fichero, el FAT puede devolver el nombre
largo (`after_epochs.ogg`) o el 8.3 (`AFTER_~1.OGG`), y entonces no coincide
con nada. Asi que:

1. se apuntan todos los `.ogg` de la carpeta mientras se recorre,
2. se compara la clave `WAVE:` con cada uno **sin distinguir mayusculas**,
3. y si no cuadra ninguno, se coge el primero: una carpeta de cancion no
   suele tener dos.

Los `.png` (las caratulas) se ignoran solos por la extension.

Por eso desaparecio `cargar_del_usb_variantes()`, que probaba el nombre en
mayusculas "a partir del septimo caracter". Ya no hace falta —los nombres
salen del listado real del directorio, con su caja exacta— y con carpetas de
por medio habria destrozado el nombre de la carpeta.

### El bit de directorio

`es_carpeta()` mira `FIO_S_ISDIR` **y** `FIO_SO_ISDIR`. Segun el driver el bit
viene en uno (0x1000, iomanX) o en el otro (0x0020, ioman). `bdmfs_fatfs` va
por iomanX, pero se miran los dos: equivocarse ahi es no ver ninguna carpeta
y no tener ni idea de por que.

### Lo que se guarda de cada cancion

Solo la cabecera: titulo y el nivel de cada uno de los 5 cursos. Las notas se
vuelven a parsear al elegir, porque un `tja_chart_t` son ~50 KB y no caben 32
en RAM. El catalogo sale ordenado por titulo.

## Poner las canciones en la imagen del pen

```sh
export MTOOLS_SKIP_CHECK=1
IMG=../USB_Ogg/usb_test.img
for d in "../Canciones prueba"/*/; do
  n=$(basename "$d")
  mmd -i $IMG "::$n"
  for f in "$d"*; do mcopy -o -i $IMG "$f" "::$n/"; done
done
mdir -i $IMG ::
```

## Compilar y probar

```sh
docker compose run --rm dev sh -c 'cd TESTS_REALES/Motor_Ritmo && make'
pcsx2-qt -batch -elf $PWD/motor.elf
```

Teclado en la config de PCSX2 de este equipo: **Cuadrado = `J` (don, rojo)**,
**Circulo = `L` (ka, azul)**.

### Los controles

Copiados del tambor de verdad (TaTaCon):

| | izquierda | derecha |
|---|---|---|
| **rojo** (don, parche central) | IZQUIERDA y ABAJO de la cruceta | CIRCULO y CRUZ |
| **azul** (ka, los bordes) | L1 | R1 |

Dos botones por color no es un capricho: es lo que permite **alternar manos**.
Con uno solo los cursos rapidos son fisicamente imposibles —en Oni hay huecos
de 100 ms y en Edit de 50— porque no da tiempo a soltar y volver a pulsar el
mismo boton.

Se acumulan en una mascara (`BOTONES_DON`, `BOTONES_KA`) y se mira el flanco
de **cualquiera** de ellos, no de uno concreto: golpear R1 con L1 todavia
hundido tiene que contar. Dos golpes del mismo color en el mismo frame cuentan
como uno, que a 50 Hz son 20 ms: muy por debajo de cualquier hueco jugable.

#### Los menus tambien se manejan con el tambor

Un tambor tiene **cuatro entradas**: dos bordes azules y dos parches rojos. No
hay cruceta, asi que ningun menu puede pedir cuatro direcciones. De ahi todo lo
demas:

| donde | azul (L1 / R1) | rojo (cualquier parche) | START |
|---|---|---|---|
| bienvenida (1a vez) | cambia de opcion | elige | nada |
| lista de canciones | sube / baja | entra en la dificultad | abre opciones |
| opciones | sube / baja | elige, o mueve la barra | cierra |
| dificultad | mueve por la fila | juega, o vuelve a la lista | nada |
| partida | golpe ka | golpe don | abre la pausa |
| pausa | cambia de opcion (3) | elige | reanuda |
| resultados | — | guarda la calibracion | vuelve al menu |

**La cruceta ya no navega**, y no es un olvido: IZQUIERDA y ABAJO son parches
rojos (`BOTONES_DON`), asi que moverse con ellos seria elegir a la vez.

Por eso la dificultad esta en **pantalla aparte** y no en la misma que la lista:
con una sola harian falta arriba/abajo para la cancion e izquierda/derecha para
la dificultad, y no hay tantos botones. Las dos pantallas van en ramas
separadas del mismo bucle, no en dos bloques seguidos: si no, el mismo golpe
rojo elegiria cancion y dificultad en el mismo fotograma y la segunda pantalla
no se llegaria a ver.

La opcion **VOLVER A LA LISTA** es una parada mas de la misma rueda que los
cinco cursos, pero se dibuja en linea aparte: las cinco columnas de 110 px
llegan hasta x=170 y una sexta se saldria de la zona segura. Se veria
perfectamente en PCSX2 y desapareceria en un tubo, que es el fallo de siempre.

**De resultados se sale con START y no con CRUZ**, aunque CRUZ fuera lo comodo
por ser la misma tecla con la que se entra. CRUZ es ahora un parche rojo: quien
siga aporreando cuando acaba la cancion se saltaria los resultados sin llegar
a verlos.

**Fallar no suena.** Antes un golpe al vacio soltaba el `SFX_CANCEL` y era justo
el ruido que sobraba. Tampoco se le pone el sonido normal de golpe, aunque un
tambor de verdad suene siempre: aqui `SFX_DON` es el aviso de "bueno" y
`SFX_BIGDON` el de "perfecto", asi que darselo a un golpe que no ha enganchado
nada borraria la unica pista de que **si** has acertado. La muestra sigue
cargada por si hace falta para otra cosa.

### Probar el ciclo sin mando

Sin ventana no hay mando que pulsar, y una sola vuelta no enseña una fuga de
hilos. Para eso:

```sh
docker compose run --rm dev sh -c 'cd TESTS_REALES/Motor_Ritmo && make auto'
pcsx2-qt -batch -elf $PWD/motor_auto.elf
```

`AUTOCICLO` elige cancion a los 50 frames de menu, corta la cancion a los 8
segundos y sale de resultados a los 50 frames: una vuelta cada ~10 s. Con
`-DAUTOGOLPE=1` ademas golpea solo en el instante de cada nota, que es la
comprobacion que de verdad discrimina: si `notas[i].resuelta` no se pusiera a
cero, la vuelta 2 daria **0 perfectos** en vez de repetir la cuenta de la
vuelta 1.

El `.ogg` se busca primero en `mass0:/click140.ogg` y `mass0:/CLICK140.OGG`; si no
hay pen, tira del empotrado en el `.elf`. Para meterlo en la imagen del USB:

```sh
MTOOLS_SKIP_CHECK=1 mcopy -o -i ../USB_Ogg/usb_test.img click140.ogg ::CLICK140.OGG
```

`click140.ogg` es un metronomo de 2 minutos a 140 BPM generado con ffmpeg, y la
chart se genera sola a ese mismo BPM: cada nota tiene que caer justo encima de un
clic. Su numero de serie es `0x96835D56`, con el bit 31 a 1, asi que **este fichero
dispara el bug del `-132`** y el `framing.o` de `libogg_fix/` a `-O1` no es
opcional (se midio en las pruebas de audio previas, que no estan en este
repo).

## Empaquetar en .iso para OPL

OPL arranca desde el disco duro imagenes ISO9660 normales. Lo unico que necesita
es un `SYSTEM.CNF` en la raiz apuntando al `.elf`, con saltos de linea CRLF:

```
BOOT2 = cdrom0:\MOTOR.ELF;1
VER = 1.00
VMODE = PAL
```

El `;1` no es decorativo: es el numero de version que ISO9660 pega a cada nombre
de fichero, y el cargador del BOOT2 lo espera. Nivel 1 obliga a nombres 8.3, que
es justo lo que hay que darle.

```sh
docker compose run --rm dev sh -c 'cd TESTS_REALES/Motor_Ritmo && make iso'
pcsx2-qt -batch -fastboot -- $PWD/motor.iso   # comprobar antes de tocar la consola
```

`xorriso` vive dentro del contenedor (`Dockerfile`); en el host no hay
ninguna herramienta de ISO, asi que la imagen hay que construirla ahi.

Aviso sobre `VMODE`: bajo OPL ese campo es practicamente inerte, OPL tiene su
propio ajuste de video por juego. Lo que manda de verdad es el
`graph_initialize(fbp, 640, 512, ...)` de `init_gs()`, que es **geometria PAL a
fuego**. En una consola NTSC habria que bajarlo a 640x448 y mover el
`draw_primitive_xyoffset` a `2048-224`.

Al arrancar de ISO no hay consola donde mirar, asi que las trazas de carga se
pintan tambien en pantalla (`scr_printf` dentro de la macro `LOG`), y se apagan
justo antes de `dma_channel_initialize` para que el log de deriva del hilo de
audio no pise el juego.

## Ficheros

| | |
|---|---|
| `motor.c` | todo el motor: catalogo, menu, partida y resultados |
| `tja.c` / `tja.h` | lector de partituras .tja |
| `sjis.c` / `sjis.h` | UTF-8 -> Shift-JIS; C portable, se compila en el PC |
| `sjis_tabla.c` | la tabla, generada; NO editar a mano |
| `gen_sjis.py` | la genera desde los rangos de KROM que usa ps2sdk |
| `prueba_sjis.c` | comprueba la conversion contra .tja de verdad |
| `Makefile` | empotra los 7 IRX, los 3 ADPCM y la cancion de reserva |
| `libogg_fix/` | `framing.c` de libogg, recompilado a `-O1` (arreglo del `-132`) |
| `click140.ogg` | metronomo de prueba, 120 s, 48 kHz estereo |
| `*.wav` | los sonidos de golpe, que `adpenc` convierte a `.adp` al compilar |

## Que falta para la demo real

1. ~~Calibrar `OFFSET_LATENCIA_MS`~~ — **hecho**: se mide con la pista
   `Metronomo (calibrar)`, se guarda en `mass0:/TATSUJI.CFG` y se lee al
   arrancar. Ya no hay que recompilar para cambiarlo.
2. ~~Parser `.tja`~~ — **hecho**: notas simples, grandes, rodillos y globos.
   Falta `#SCROLL`, `#BRANCHSTART` y `#DELAY`.
3. ~~Mapeo a 4 botones~~ — **hecho**, ver "Los controles". Con el ya se puede
   alternar manos, que es lo que hacia falta para Oni y Edit. Lo que queda de
   aqui es que las **notas grandes pidan los dos parches del mismo color a la
   vez**: ahora valen con uno solo.
4. ~~Recorrer las carpetas del pen~~ — **hecho**, ver arriba. Queda medir en
   la consola cuanto cuesta abrir un fichero: si el coste estuviera por abrir
   y no por byte, con 40 canciones habria que guardar un indice en el pen en
   vez de leer la cabecera de cada `.tja` en vivo. El menu ya enseña el dato
   (`pen: N KB abrir N ms leer N ms`).
5. ~~Pausa reanudable~~ — **hecha**, con cuenta atras de 3 segundos.
6. ~~Guardar la calibracion~~ — **hecho**, con pantalla de bienvenida la
   primera vez. ~~Volumen~~ — **hecho**, musica y sonido en el mismo fichero,
   con menu de opciones y borrado de perfil. ~~Puntuaciones~~ — **hechas**, un
   `PUNTOS.CFG` por carpeta de cancion.
7. ~~Titulos en japones~~ — **hecho**, ver arriba: KROM en `DOUBLE_BYTE` y
   conversion a Shift-JIS. Lo que queda es que los kanji de **JIS nivel 2** no
   estan en la consola y salen como `?`.
8. ~~Puntuacion~~ — **hecha**, reparto shin'uchi. ~~Alma~~ — **hecha**, con
   norma por dificultad y por nivel. Las dos, arriba.
9. **Notas grandes con un solo golpe** (ver arriba): en el taiko de verdad
   piden los dos parches del mismo color a la vez. Ojo, no es un fallo grave:
   alli tambien cuentan dandoles con una mano, solo que sin bonus, y con el
   reparto shin'uchi las grandes no valen doble de todas formas.

10. ~~`#SCROLL`, `#GOGOSTART/END`, `#DELAY`, `#BRANCHSTART`~~ — **hechos**,
    ver "Los comandos del .tja" mas abajo. De las bifurcaciones se lee **una
    sola rama** (la primera, que es la normal): no se evalua la condicion.

11. ~~Topes~~ — `MAX_CANCIONES` **256** y `MAX_NOTAS` **8192**. Queda
    `PROF_MAX` 2 niveles de carpetas, y que `LEVEL:11.7` se lee como 11 (para
    la norma del alma da igual).

12. **No hay dibujo de verdad**: circulos y rectangulos. Ni fondo, ni
    personaje, ni texturas de nota. Y en el menu no suena la muestra de la
    cancion, aunque el `.tja` trae el `DEMOSTART` para eso.
