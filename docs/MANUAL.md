# MSXFileForge — Manual de usuario

MSXFileForge es un editor e inspector de ficheros de MSX: imágenes de disquete
(`.dsk`, `.dmk`), discos duros y tarjetas SD (MSX-DOS 2 / Nextor), cartuchos
(`.rom`) y cintas (`.cas`, `.tsx`, `.wav`). Tiene dos formas de uso que hacen
exactamente lo mismo por debajo:

- **`msxforge-gui`**: aplicación de escritorio con ventanas.
- **`msxforge`**: línea de comandos, para scripts, lotes y servidores.

Este manual cubre ambas. La sección 6 reúne recetas para las tareas habituales.

---

## Índice

1. [Instalación](#1-instalación)
2. [Conceptos básicos](#2-conceptos-básicos)
3. [La aplicación gráfica](#3-la-aplicación-gráfica)
4. [La línea de comandos](#4-la-línea-de-comandos)
5. [Discos duros y tarjetas SD](#5-discos-duros-y-tarjetas-sd)
6. [Recetas](#6-recetas)
7. [Límites conocidos](#7-límites-conocidos)
8. [Solución de problemas](#8-solución-de-problemas)

---

## 1. Instalación

### Binarios ya compilados

Cada cambio en `master` genera binarios para **Windows**, **Linux**, **macOS
Intel** y **macOS Apple Silicon** en la pestaña **Actions** del repositorio
(artefactos `msxforge-windows`, `msxforge-linux`, `msxforge-macos-x86_64`,
`msxforge-macos-arm64`). Cada paquete trae `msxforge`, `msxforge-gui` y
`dsk2dmk`.

- **Windows**: descomprime y ejecuta `msxforge-gui.exe`. `SDL2.dll` va incluida.
- **Linux**: necesita SDL2 del sistema (`sudo apt install libsdl2-2.0-0`). Los
  diálogos de abrir/guardar usan `zenity` o `kdialog`.
- **macOS**: los binarios no están firmados. Si Gatekeeper se niega a abrirlos:
  `chmod +x msxforge*` y `xattr -cr msxforge-gui` (y lo mismo con `msxforge`).

### Compilar desde el código

Hace falta un compilador C++17 y CMake; para la GUI, además, SDL2 y OpenGL.

```sh
# Linux / macOS, todo (GUI incluida)
./build-unix.sh

# Solo núcleo + CLI + tests, sin SDL2 ni OpenGL (servidores, CI)
cmake -B build -DMSXFF_BUILD_GUI=OFF && cmake --build build -j

# Comprobar que todo funciona
ctest --test-dir build --output-on-failure
```

En Windows: `./build.sh` (MinGW) o `cmake -B build && cmake --build build --config Release` (MSVC).

---

## 2. Conceptos básicos

### Qué ficheros abre

| Tipo | Extensiones | Qué se puede hacer |
|---|---|---|
| Disquete | `.dsk` (volcado plano de sectores, 360/720 KB) | Leer y escribir el sistema de ficheros |
| Disquete con protección | `.dmk` (imagen de pistas) | Leer y escribir; ver sectores raros, marcas borradas y errores de CRC |
| Disco duro / SD | `.img`, `.dsk`, `.hdd`… (cualquier imagen grande) | FAT12/FAT16, con o sin tabla de particiones |
| Cartucho | `.rom`, `.mx1`, `.mx2` | Cabecera `AB`, puntos de entrada y mapper probable |
| Cinta | `.cas`, `.tsx`/`.tzx`, `.wav` | Lista de bloques; conversión entre los tres formatos |

El tipo de disco se detecta por el contenido, no por la extensión, salvo el
`.dmk`, que se reconoce por su extensión (si un `.dmk` resulta ser un volcado
plano, se lee como tal).

### Sistema de ficheros: FAT12 y FAT16

MSX-DOS usa FAT12 en disquetes; MSX-DOS 2 y Nextor usan FAT12 o FAT16 en discos
duros y SD. MSXFileForge elige el tipo como lo hace MS-DOS: por el número de
clusters (FAT16 a partir de 4085). **FAT32 no está soportado**: el MSX no lo usa,
y la herramienta lo dice claramente en vez de leer basura.

### Nombres 8.3

Los nombres de MSX-DOS tienen hasta 8 caracteres, un punto opcional y hasta 3 de
extensión (`AUTOEXEC.BAS`), siempre en mayúsculas. No se admiten espacios ni
`" * + , . / : ; < = > ? [ \ ] |`.

- Si **escribes tú el nombre** (renombrar, crear carpeta, `add` con NAME) y no es
  válido, se rechaza con el motivo.
- Si el nombre **viene de un fichero del PC** (Add file, `pack`, `add` sin NAME),
  se acorta automáticamente: `Mi programa largo.bas` → `MIPROG~1.BAS`. Si ya
  existe, se usa `~2`, `~3`…

### Los cambios no se guardan solos

Toda edición se hace sobre una copia en memoria. En la GUI no se toca el fichero
hasta **File > Save image** (`Ctrl+S`). En la CLI, cada comando que modifica
guarda al terminar, sobre la misma imagen o sobre la indicada como `outimage`.
Si activas **File > Keep .bak on save**, se guarda una copia del original.

### Datos RAW (sin nombre)

Muchos juegos (Fray, Xak…) muestran 1 o 2 ficheros pero el disco está casi
lleno. El resto son **datos RAW**: clusters marcados como ocupados en la FAT pero
sin entrada de directorio, que el cargador del juego lee por sector. **No es
cifrado**: los datos se pueden leer, exportar, editar y volver a importar.

---

## 3. La aplicación gráfica

### Abrir

- **File > Open** (`Ctrl+O`), arrastrar el fichero a la ventana, o escribir la
  ruta en la barra superior y pulsar **Open path**.
- **File > New blank disk** crea un disquete vacío de 720 KB o 360 KB.
- **File > Close** cierra la imagen actual.

Bajo la barra de ruta aparece una línea de información: formato, geometría (en
disquetes) o tamaño (en discos duros), si hay protección, tipo de FAT, OEM y
espacio libre.

### Panel izquierdo: el árbol de ficheros

- Las carpetas salen en amarillo con `[D]`; clic para entrar, **.. (up)** para
  subir.
- **Show deleted** muestra también las entradas borradas (en rojo).
- **Partition** (solo en discos con particiones) elige qué partición ver.
- Botones:
  - **Add file…**: copia un fichero del PC a la carpeta actual (nombre 8.3
    automático).
  - **New folder**: crea una subcarpeta.
  - **Rename**: renombra el fichero seleccionado.
  - **Remove this folder**: borra la carpeta en la que estás (tiene que estar
    vacía) y vuelve a la de arriba.
  - **Refresh**: vuelve a leer el directorio.

### Panel derecho: el fichero seleccionado

Arriba: nombre, tamaño, tipo detectado y CRC32. Botones:

- **Export to file…**: guarda el fichero en el PC.
- **Import & replace…**: sustituye su contenido por un fichero del PC (la cadena
  de clusters crece o encoge sola).
- **Delete file**: lo borra y libera sus clusters.
- **Undelete** (solo en ficheros borrados): escribe en la casilla la primera
  letra, que MSX-DOS pierde al borrar, y pulsa el botón. Funciona si los
  clusters del fichero eran contiguos y no se han reutilizado (como `UNDEL` de
  MSX-DOS).

Pestañas:

| Pestaña | Para qué sirve |
|---|---|
| **BASIC (edit)** | Un programa BASIC tokenizado se muestra como listado normal. Edítalo y pulsa **Save to image**: se vuelve a tokenizar. |
| **Text (edit)** | Ficheros de texto (incluido BASIC guardado con `SAVE "x",A`). |
| **BLOAD** | Ficheros binarios de `BSAVE`: direcciones de inicio, fin y ejecución. |
| **Hex (edit)** | Clic en un byte para cambiarlo, o usa la casilla offset/valor. Los bytes cambiados se marcan en amarillo. **Save to image** escribe. |
| **Raw image** | Vista hexadecimal de la imagen completa. |
| **RAW data / map** | Mapa de clusters (verde = con nombre, naranja = RAW, gris = libre; pasa el ratón por encima para ver el cluster y su posición). Exportar/importar los datos RAW, exportar la imagen y **Carve**: trocear los datos RAW en bloques BLOAD, programas BASIC y huecos, y exportar cada uno. |
| **Protection / sectors** | Tabla de sectores físicos (sobre todo en `.dmk`): tamaños no estándar, marcas de datos borrados, errores de CRC e IDs duplicados, en naranja. |

**Revert** (o **Edit > Revert file**) descarta los cambios del fichero sin
guardar.

### Menús

- **File**: Open, New blank disk, Save image (`Ctrl+S`), Save image as
  (`Ctrl+Shift+S`), Keep .bak on save, Close, Quit.
- **Edit**: Revert file, Delete file.
- **Tools**: **Check filesystem (fsck)** comprueba la integridad: clusters
  compartidos por dos ficheros, cadenas rotas, tamaños que no cuadran con la
  cadena, clusters perdidos. **Open in openMSX** lanza el emulador con el disco
  (tiene que estar instalado y en el PATH).
- **Help**: Manual (`F1`) y About.

Los `(?)` repartidos por la ventana muestran ayuda al pasar el ratón.

### ROM y cintas

Al abrir un `.rom` se ve el tamaño, las páginas de 16 KB, la cabecera `AB`, los
puntos de entrada (INIT, STATEMENT, DEVICE, TEXT) y el mapper probable (Konami,
Konami SCC, ASCII8, ASCII16 o ninguno). Un `.cas`, `.tsx` o `.wav` muestra sus
bloques y nombres de fichero.

---

## 4. La línea de comandos

```
msxforge <comando> <fichero> [argumentos] [--part N]
```

Ejecuta `msxforge` sin argumentos para ver la lista. Reglas comunes:

- Donde pone `<FILE>` puedes usar una ruta: `JUEGOS/SAVE/SLOT1.DAT`, con `/` o
  `\`, sin distinguir mayúsculas.
- `--part N` elige la partición N de un disco duro, en cualquier posición de la
  línea. Sin ella se usa la primera partición FAT.
- Los comandos que modifican guardan sobre la misma imagen, salvo que aceptes y
  des un `outimage`.

### Consultar

| Comando | Qué hace |
|---|---|
| `info <img>` | Formato, geometría, protección, particiones, tipo de FAT, etiqueta y espacio libre. |
| `ls <img> [/RUTA] [--deleted]` | Lista una carpeta (con `--deleted`, también lo borrado). |
| `cat <img> <FILE>` | Vuelca los bytes del fichero a la salida estándar. |
| `detok <img> <FILE>` | Muestra un BASIC tokenizado como texto. |
| `crc <img> [FILE]` | CRC32 de la imagen o de un fichero de dentro. |
| `fsck <img>` | Comprueba la integridad del sistema de ficheros (código 1 si hay problemas). |
| `diff <imgA> <imgB>` | Compara todos los ficheros de dos discos por CRC (código 1 si difieren). |
| `protect <img>` | Inspector de sectores físicos y protecciones. |
| `label <img>` | Muestra la etiqueta de volumen. |
| `attrib <img> <FILE>` | Muestra los atributos (`R` solo lectura, `H` oculto, `S` sistema, `D` carpeta, `A` archivo). |

### Sacar ficheros

| Comando | Qué hace |
|---|---|
| `extract <img> <FILE> <destino>` | Copia un fichero al PC. |
| `unpack <img> <carpeta>` | Extrae el árbol completo a una carpeta del PC. |
| `raw <img> [salida.bin]` | Mapa de clusters (con nombre / RAW / libres); con `salida.bin`, vuelca los datos RAW. |
| `carve <img>` | Trocea los datos RAW en bloques BLOAD, BASIC y huecos. |

### Modificar

| Comando | Qué hace |
|---|---|
| `add <img> <fichero_pc> [/CARPETA] [NOMBRE]` | Añade un fichero nuevo (sin NOMBRE, se deriva un 8.3 del nombre del PC). |
| `put <img> <FILE> <fichero_pc> [outimage]` | Sustituye el contenido de un fichero existente. |
| `del <img> <FILE\|CARPETA> [outimage]` | Borra un fichero o una carpeta vacía. |
| `undelete <img> <[CARPETA/]?NOMBRE> [LETRA] [outimage]` | Recupera un fichero borrado. Como la primera letra se pierde, pon `?` en su lugar y da la letra aparte: `undelete d.dsk ?AME.BIN G`. |
| `rename <img> <FILE> <NUEVO>` | Renombra en su sitio. |
| `mkdir <img> <NOMBRE> [/PADRE]` | Crea una carpeta. |
| `attrib <img> <FILE> +R -H +S -A …` | Activa (`+`) o quita (`-`) atributos. |
| `label <img> <ETIQUETA>` / `label <img> --clear` | Pone o quita la etiqueta de volumen (hasta 11 caracteres). |
| `rawput <img> <fichero_pc> [outimage]` | Escribe datos de vuelta en la zona RAW (código 8 si no caben enteros). |

### Crear imágenes

| Comando | Qué hace |
|---|---|
| `format <salida> [360\|720\|<N>M] [ETIQUETA]` | Disquete vacío de 360/720 KB, o volumen FAT16 sin particiones de N MB (`32M`). |
| `pack <carpeta> <salida.dsk> [360\|720] [--label L]` | Crea un disquete con todo el contenido de una carpeta del PC, subcarpetas incluidas. |
| `mkhdd <salida.img> <N>M[,<N>M…]` | Disco duro / SD con tabla de particiones: `mkhdd sd.img 32M,32M,16M`. |
| `tok <listado.txt> [salida.bas]` | Tokeniza un listado BASIC en texto (y muestra el resultado detokenizado para comprobar). |
| `dsk2dmk <in.dsk> <out.dmk>` | (programa aparte) convierte un `.dsk` en `.dmk` con CRC válidos. |

### Cartuchos y cintas

| Comando | Qué hace |
|---|---|
| `rominfo <rom>` | Cabecera, puntos de entrada y mapper probable. |
| `cas <cas>` | Lista de bloques de la cinta. |
| `cas2wav <in.cas> <out.wav> [-2]` | Genera el audio para grabar en cinta o reproducir en un MSX real (`-2` = 2400 baudios). |
| `cas2tsx <in.cas> <out.tsx> [-2]` | Convierte a TSX. |
| `tsx2cas <in.tsx> <out.cas>` | Extrae los datos de un TSX/TZX. |
| `wav2cas <in.wav> <out.cas>` | Decodifica un WAV limpio (los volcados de cinta ruidosos pueden fallar). |

### Códigos de salida

| Código | Significado |
|---|---|
| 0 | Correcto |
| 1 | Uso incorrecto; o `fsck` encontró problemas; o `diff` encontró diferencias |
| 2 | No se puede leer la entrada (o escribir la imagen nueva) |
| 3 | No hay sistema de ficheros montable (o la partición no existe) |
| 4 | Fichero o carpeta no encontrado |
| 5 | No se puede escribir en el PC |
| 6 | La operación no se pudo hacer (disco lleno, nombre no válido, carpeta no vacía…) |
| 7 | No se pudo guardar la imagen |
| 8 | `rawput`: los datos no cabían enteros |

---

## 5. Discos duros y tarjetas SD

Las imágenes de discos duros y de tarjetas SD de MSX-DOS 2 / Nextor suelen tener
una tabla de particiones MBR: una partición primaria y, a menudo, una partición
extendida con más unidades lógicas. MSXFileForge las lee todas.

```sh
msxforge info sd.img                  # lista las particiones
msxforge ls sd.img --part 2           # ver la segunda
msxforge add sd.img juego.rom /JUEGOS --part 2
```

En la GUI, el selector **Partition** aparece encima del árbol de ficheros.

Para crear imágenes nuevas:

```sh
msxforge mkhdd sd.img 32M,32M,16M     # 3 particiones (con 5 o más: 1 primaria + lógicas)
msxforge format sd.img 64M NEXTOR     # un solo volumen FAT16, sin tabla de particiones
```

Hasta 2047 MB por imagen y 255 particiones. Las particiones de 2 MB o menos salen
como FAT12 y las mayores como FAT16, que es lo que espera MSX-DOS 2.

---

## 6. Recetas

**Meter un juego de una carpeta del PC en un disquete:**

```sh
msxforge pack MiJuego/ mijuego.dsk 720 --label MIJUEGO
msxforge fsck mijuego.dsk
```

**Sacar todo lo que hay en un disco:**

```sh
msxforge unpack disco.dsk extraido/
```

**Editar un programa BASIC en el PC y devolverlo al disco:**

```sh
msxforge detok disco.dsk JUEGO.BAS > juego.txt
#   ...editar juego.txt...
msxforge tok juego.txt JUEGO.BAS
msxforge put disco.dsk JUEGO.BAS JUEGO.BAS
```

En la GUI es más corto: pestaña **BASIC (edit)**, editar, **Save to image** y
`Ctrl+S`.

**Recuperar un fichero borrado por error:**

```sh
msxforge ls disco.dsk --deleted       # aparece como ?UEGO.BAS
msxforge undelete disco.dsk ?UEGO.BAS J
```

Hazlo cuanto antes: si después se escriben ficheros nuevos, pueden ocupar sus
clusters y la recuperación se rechaza para no devolver datos mezclados.

**Ver qué cambió entre dos versiones de un disco:**

```sh
msxforge diff original.dsk modificado.dsk
```

**Estudiar un disco con protección o datos RAW:**

```sh
msxforge info juego.dmk               # ¿protegido?
msxforge protect juego.dmk            # sectores raros, CRC, marcas borradas
msxforge raw juego.dmk datos.bin      # vuelca los datos sin nombre
msxforge carve juego.dmk              # ¿qué bloques BLOAD/BASIC contienen?
```

**Pasar un `.cas` a audio para cargarlo en un MSX real:**

```sh
msxforge cas2wav juego.cas juego.wav  # en el MSX: RUN"CAS:" / BLOAD"CAS:",R / CLOAD
```

**Preparar una SD para Nextor con dos unidades y copiar juegos:**

```sh
msxforge mkhdd sd.img 512M,512M
msxforge mkdir sd.img JUEGOS --part 1
msxforge add sd.img juego.rom /JUEGOS --part 1
```

La imagen resultante se graba en la tarjeta con cualquier programa de volcado de
imágenes (Win32 Disk Imager, `dd`, balenaEtcher…).

---

## 7. Límites conocidos

- **Nextor real**: el soporte de discos duros está probado con imágenes creadas
  por la propia herramienta, no todavía con tarjetas particionadas por el FDISK
  de Nextor.
- **Discos protegidos**: las protecciones de `.dmk` (CRC, IDs duplicados, marcas
  borradas, sectores fuera de pista) se prueban con imágenes construidas, no con
  una colección de originales comerciales. Los sectores cuyos datos se salen de
  la pista se pueden leer, pero no escribir.
- **Tamaño**: las imágenes se cargan enteras en memoria. En Windows las
  posiciones son de 32 bits, así que conviene no pasar de 2 GB; `format` y
  `mkhdd` crean como máximo 2047 MB.
- **FAT32** no está soportado (MSX-DOS y Nextor no lo usan).
- **BASIC**: los números se convierten de forma exacta. Al listar, las
  constantes de precisión simple llevan `!` y las de doble precisión que
  parecerían enteros llevan `#`, para que al volver a tokenizar salgan los mismos
  bytes; el `LIST` de un MSX real puede mostrar alguna sin ese sufijo.
- **Undelete** solo recupera ficheros (no carpetas) y supone que sus clusters
  eran contiguos, igual que `UNDEL` de MSX-DOS.
- **WAV → CAS** solo es fiable con audio limpio, como el que genera `cas2wav`.
- El directorio raíz tiene tamaño fijo (112 entradas en disquetes estándar, 512
  en volúmenes FAT16); las subcarpetas crecen sin límite práctico.

---

## 8. Solución de problemas

| Mensaje | Qué significa / qué hacer |
|---|---|
| `mount error: not a FAT boot sector` | El sector de arranque no describe un sistema de ficheros y tampoco se pudo deducir del byte de medio. Puede ser un disco de juego con formato propio: usa `info`, `protect` y la pestaña **Raw image**. |
| `FAT32 volumes are not supported` | Formatea el volumen como FAT16 (`format x.img 512M`). |
| `no partition N (the image has M)` | Revisa los números con `info`. |
| `invalid 8.3 name …` | El nombre que escribiste no cumple 8.3 (sección 2). Si viene de un fichero del PC, omite NOMBRE y se acortará solo. |
| `disk full (no free clusters)` | No hay sitio. La operación no ha cambiado nada en el disco. |
| `directory is full` | El directorio raíz tiene tamaño fijo; crea una subcarpeta (`mkdir`) y guarda ahí. |
| `cluster N has been reused…` | Undelete imposible: otro fichero ocupa ya parte de los datos. |
| `directory is not empty` | Borra antes su contenido. |
| `fsck` muestra `cross-linked` | Dos ficheros comparten clusters: el disco ya venía dañado. Extrae lo que puedas con `unpack` antes de tocarlo. |
| La GUI no abre en macOS | `xattr -cr msxforge-gui` (binario sin firmar). |
| Los diálogos de abrir/guardar no salen en Linux | Instala `zenity` o `kdialog`, o escribe la ruta en la barra superior. |

---

MSXFileForge es software libre (GPL-3.0-or-later). Errores y sugerencias:
<https://github.com/VC-Anvil/MSXFileForge/issues>.
