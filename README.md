# Vib-Tatsuji

Un motor de juego de ritmo tipo **Taiko no Tatsujin** para **PlayStation 2**,
escrito en C con ps2sdk. Lee las canciones de un **pen USB**, con el formato
`.tja` de open-taiko, y se juega con el tambor oficial (TaTaCon) o con mando.

Corre en consola de verdad, no solo en emulador.

## Que hay hecho

- **Reloj de cancion sacado del audio**, no de contar fotogramas. Las notas se
  colocan por milisegundos y el acierto se juzga por tiempo, asi que un
  fotograma perdido no descuadra la partitura.
- **Lector de `.tja`**: notas normales y grandes, rodillos, globos,
  `#BPMCHANGE`, `#MEASURE`, `#SCROLL`, `#GOGOSTART`, `#DELAY` y las partituras
  bifurcadas (de las que se lee una rama).
- **Recorrido del pen**: una carpeta por cancion, como las deja open-taiko.
- **Los cuatro tipos de nota** jugables, con las ventanas de juicio del juego
  de verdad y distintas por dificultad.
- **Puntuacion shin'uchi**: la partitura entera vale un millon.
- **Alma** (魂ゲージ) con la norma por dificultad y por nivel.
- **Titulos en japones**, tirando de las fuentes de la BIOS de la consola.
- Menus navegables **solo con el tambor**, pausa reanudable con cuenta atras,
  calibracion de latencia, volumenes y puntuaciones guardadas en el propio pen.

## Que le falta

- Las **notas grandes** valen con un solo golpe; en el juego de verdad piden
  los dos parches del mismo color a la vez.
- Estilo y estetica del juego, ahora mismo esta lo justo y necesario para jugar
- No suena la muestra de la cancion en el menu, aunque el `.tja` trae el
  `DEMOSTART` para eso.

## Compilar

Hace falta Docker; el contenedor trae el toolchain de ps2dev.

```sh
docker compose run --rm dev sh -c 'make'      # motor.elf
docker compose run --rm dev sh -c 'make iso'  # motor.iso, para OPL
```

## Probar

En consola, con **OPL** desde el disco duro: copia `motor.iso` a tu carpeta de
DVD/CD.

En emulador:

```sh
pcsx2-qt -batch -elf $PWD/motor.elf
```

La ruta **absoluta** importa: PCSX2 no resuelve las relativas, arranca sin ELF
y aborta en el recompilador con un mensaje que no dice nada de eso.

## Poner canciones

En la raiz del pen, una carpeta por cancion con su `.tja` y su `.ogg` dentro,
que es como las deja open-taiko:

```
mass0:/
  Mi Cancion/
    mi cancion.tja
    mi cancion.ogg
```

Se admite tambien una capa mas de carpetas (las de genero) y `.tja` sueltos en
la raiz. El audio tiene que ser **Ogg Vorbis**; los `.tja` valen en UTF-8 o en
Shift-JIS.

En este repo **no hay ninguna cancion**: las de prueba son musica con derechos.

La primera vez que arranca, el juego te ofrece calibrar la latencia con el
metronomo. Lo que salga se guarda en `TATSUJI.CFG` **en el propio pen**, junto a
las canciones, y las puntuaciones en un `PUNTOS.CFG` dentro de la carpeta de
cada una: asi tus datos viajan con el pen.

## Los controles

Copiados del tambor de verdad:

| | izquierda | derecha |
|---|---|---|
| **rojo** (don, parche central) | IZQUIERDA y ABAJO de la cruceta | CIRCULO y CRUZ |
| **azul** (ka, los bordes) | L1 | R1 |

Dos botones por color no es un capricho: es lo que permite alternar manos, y sin
eso los cursos rapidos son fisicamente imposibles.

Los menus se manejan igual: **azul** mueve, **rojo** elige. START abre las
opciones en el selector, y la pausa durante la cancion.

## Los ficheros

| | |
|---|---|
| `motor.c` | el motor entero: catalogo, menus, partida y resultados |
| `tja.c` / `tja.h` | lector de partituras `.tja` |
| `sjis.c` / `sjis.h` | UTF-8 a Shift-JIS para los titulos japoneses |
| `sjis_tabla.c` | tabla generada; la hace `gen_sjis.py`, no editar a mano |
| `prueba_tja.c` / `prueba_sjis.c` | comprobaciones que se compilan en el PC |
| `libogg_fix/` | `framing.c` de libogg recompilado a `-O1` (ver DISENO.md) |

## Por que esta hecho asi

En **[DISENO.md](DISENO.md)** esta lo que se midio, lo que se probo y lo que
salio mal antes de llegar aqui: por que el reloj sale del audio y no de los
frames, por que ningun hilo puede esperar en vacio, por que hubo que recompilar
un fichero de libogg, de donde salen las ventanas de juicio y las cuentas de la
puntuacion, y unas cuantas trampas mas que costaron horas.

Si vas a tocar el codigo, empieza por ahi.

## Gracias a

- [ps2dev/ps2sdk](https://github.com/ps2dev/ps2sdk), que es lo que hace que
  esto sea posible.
- [OpenTaiko](https://github.com/0auBSQ/OpenTaiko), de donde salen —leyendo el
  codigo— las ventanas de juicio, el reparto de puntos y las cuentas del alma.
