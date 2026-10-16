# File Manager — explorador de archivos para Game & Watch (Retro-Go SD)

[English](README.md) | **Español**

Homebrew GWHB para el firmware Retro-Go SD 2.x (rama `external_cores`) que permite manejar la
tarjeta SD desde la propia consola: copiar, mover, renombrar, borrar, ver y editar archivos.

## Qué hace

- **Navegar** por toda la SD, con las carpetas primero, en orden alfabético y con los tamaños.
- **Copiar, cortar (mover) y pegar** archivos y **carpetas completas**, con todo su contenido.
- **Renombrar**, **borrar** (con confirmación), **crear carpetas** y **crear archivos de texto**.
- **Selección múltiple**: las acciones se aplican a todo lo seleccionado.
- **Ver** archivos de texto, en hexadecimal e **imágenes**: JPEG, PNG, GIF (también animados) y BMP
  (también las capturas del firmware).
- **Ver vídeos**: AVI con vídeo Motion-JPEG y sonido PCM o MP3 (los tuyos se convierten con un comando de
  ffmpeg).
- **Escuchar música**: MP3 (también MP2), FLAC, WAV y AIFF, siguiendo con el siguiente archivo de la carpeta.
- **Editar texto** (`.txt`, `.cfg`, `.ini`…) con un teclado en pantalla.
- **Editar en hexadecimal** cualquier archivo, con un teclado hexadecimal.
- **Información** de archivos y carpetas, y de la **tarjeta SD** (capacidad, usado, libre).

Los textos salen en español o en inglés según el idioma del firmware (cualquier otro idioma usa
el inglés).

## Instalación

Copia `FileManager.bin` a `/homebrews/` en la SD. Aparece en la pestaña **Homebrew** como
"File Manager".

---

# Manual

## 1. La lista de archivos

La pantalla principal muestra la carpeta actual: arriba la ruta, en medio el contenido (carpetas
en amarillo y con `/`, archivos con su tamaño), y abajo la posición del cursor, cuántos elementos
hay seleccionados (`*3`) y lo que hay en el portapapeles (`[2 para copiar]`).

| Botón | Acción |
|---|---|
| Arriba / abajo | Moverse por la lista |
| Izquierda / derecha | Página anterior / siguiente |
| A | Entrar en la carpeta, o abrir el archivo (ver más abajo qué visor usa) |
| B | Volver a la carpeta de arriba |
| TIME | Seleccionar / quitar selección del elemento (y baja al siguiente) |
| GAME | Menú de acciones |
| PAUSE/SET | Menú de Retro-Go (brillo, volumen, salir, apagar) |
| PAUSE/SET + cruceta | Atajos de brillo / volumen, con su indicador en pantalla |

Al pulsar **A** sobre un archivo:
- `.jpg`, `.png`, `.gif`, `.bmp` → visor de imágenes; también las carátulas `.img` de Retro-Go
  (`/covers/...`), reconocidas por su contenido (otros `.img`, como imágenes de disco, se abren en hex);
- `.avi` → reproductor de vídeo (`.mp4`, `.mkv`, `.mov`… explican cómo convertirlos);
- `.mp3`, `.mp2`, `.flac`, `.wav`, `.aiff` → reproductor de audio (`.ogg`, `.m4a`… explican cómo convertirlos);
- `.txt`, `.cfg`, `.ini`, `.log`, `.json`, `.xml`, `.csv`, `.md`, `.c`, `.h`… → visor de texto;
- cualquier otro → visor hexadecimal.

## 2. El menú de acciones (GAME)

Las opciones cambian según dónde esté el cursor y si hay algo seleccionado o en el portapapeles:

| Opción | Qué hace |
|---|---|
| Abrir | Igual que pulsar A |
| Ver como texto | Abre el archivo en el visor de texto, sea de la extensión que sea |
| Editar texto | Abre el editor de texto (apartado 5) |
| Ver / editar en hexadecimal | Abre el visor hexadecimal (apartado 6) |
| Copiar / Cortar (mover) | Pone en el portapapeles el elemento del cursor, o todos los seleccionados |
| Pegar aquí (n) | Copia o mueve aquí lo que hay en el portapapeles |
| Renombrar | Cambia el nombre con el teclado de nombres (apartado 4) |
| Borrar | Borra el elemento o los seleccionados, carpetas con todo su contenido. Pide confirmación y por defecto marca "No" |
| Nueva carpeta | Crea una carpeta en la carpeta actual |
| Nuevo archivo de texto | Crea un archivo vacío y lo abre en el editor |
| Información | Tamaño y fecha; en carpetas, cuántos archivos y subcarpetas tiene y cuánto ocupan |
| Seleccionar todo / Quitar selección | Selecciona o deselecciona todo lo de la carpeta |
| Vaciar portapapeles | Olvida lo copiado o cortado |
| Tarjeta SD | Capacidad total, espacio usado y libre (necesita el firmware nuevo, ver "Firmware") |
| Salir | Vuelve al menú de Retro-Go |

## 3. Copiar, mover y pegar

1. Selecciona lo que quieras con **TIME** (o pon el cursor encima si es una sola cosa).
2. **GAME → Copiar** o **Cortar**.
3. Ve a la carpeta de destino y **GAME → Pegar**.

- Sale una barra de progreso, y **B cancela**. Nunca queda un archivo copiado a medias.
- Si ya existe algo con el mismo nombre, pregunta: **sobrescribir**, **omitir** o **cancelar**.
- Si pegas una copia en la misma carpeta, crea `nombre (2).ext`.
- No deja copiar una carpeta dentro de sí misma.
- Mover es instantáneo con el firmware nuevo. Con el antiguo, copia y luego borra el original.

## 4. El teclado de nombres (renombrar, nueva carpeta, nuevo archivo)

Es un teclado a pantalla completa con el nombre arriba:

| Botón | Acción |
|---|---|
| Cruceta | Moverse por las teclas |
| A | Escribir la tecla |
| B | Borrar el último carácter |
| TIME | Cambiar entre minúsculas, mayúsculas y símbolos |
| GAME | Aceptar |
| PAUSE | Cancelar |

En la fila de abajo también tiene **Espacio**, **Borrar**, **abc/ABC**, **OK** y **Cancelar**. No
deja aceptar nombres con caracteres que no admite la tarjeta (`\ / : * ? " < > |`).

## 5. El editor de texto

Se abre con **GAME → Editar texto**, con **A** dentro del visor de texto, o al crear un archivo
nuevo. Arriba se ve el texto, con el nombre del archivo y la línea:columna del cursor en el título
(con `*` si hay cambios sin guardar). Abajo hay un teclado completo.

El editor tiene dos modos, que se cambian con **TIME**:

| Botón | Con el teclado | Sin el teclado (modo cursor) |
|---|---|---|
| Cruceta | Elegir tecla | Mover el cursor por el texto |
| A | Pulsar la tecla elegida | Volver a mostrar el teclado |
| B | Borrar el carácter anterior | Borrar el carácter anterior |
| TIME | Ocultar el teclado (se ven más líneas) | Mostrar el teclado |
| GAME | Menú del editor | Menú del editor |

**Teclas especiales:** Borra (retroceso), Supr (borra el carácter del cursor), Intro (nueva
línea), Tab, Espacio, flechas `< > ^ v`, Ini/Fin (principio/final de línea) y RePag/AvPag.
**Mayús**: una pulsación (verde) pone en mayúsculas solo la siguiente tecla; dos (rojo, sale
`MAYUS`) la dejan fija; tres la quitan.

**Menú del editor (GAME):** Guardar, Guardar y salir, o Salir sin guardar (pide confirmación). Si
no hay cambios, solo ofrece Salir.

Detalles:
- Las líneas no se cortan: si son más largas que la pantalla, la vista se desplaza hacia el lado.
- Respeta los finales de línea de Windows (CRLF) y los caracteres con tilde o ñ del archivo. En
  pantalla salen sin tilde porque la letra pequeña no las tiene, pero se guardan intactos.
- Al guardar escribe primero una copia temporal y luego sustituye el original, así que un fallo
  no deja el archivo a medias. Con el firmware antiguo (sin renombrado rápido) escribe
  directamente sobre el archivo.
- Tamaño máximo para editar: 160 KB.

## 6. El visor y editor hexadecimal

Muestra cada línea con la posición (en gris), 8 bytes en hexadecimal y los mismos 8 bytes como
texto a la derecha.

**Viendo:** cruceta arriba/abajo para desplazarse, izquierda/derecha para cambiar de página,
**A** para editar y **B** para salir.

**Editando:** aparece un cursor naranja. La cifra que se va a escribir sale en rojo. Abajo hay un
teclado hexadecimal:

```
 0 1 2 3 4 5 6 7  -1 +1
 8 9 A B C D E F  Borra
 < > ^ v  RePag AvPag  Texto
```

| Botón | Con el teclado | Sin el teclado (modo cursor) |
|---|---|---|
| Cruceta | Elegir tecla | Mover el cursor (izquierda/derecha de cifra en cifra) |
| A | Pulsar la tecla elegida | Volver a mostrar el teclado |
| B | Retroceder una cifra y devolverla a su valor original | Igual |
| TIME | Ocultar el teclado | Mostrar el teclado |
| GAME | Menú: Guardar, Guardar y salir, Salir sin guardar | Igual |

- **0-9 y A-F** escriben la cifra donde está el cursor y avanzan solas a la siguiente, como en un
  editor hexadecimal de PC. Para escribir `FF 00 12` basta con pulsar F, F, 0, 0, 1, 2.
- **-1 / +1** restan o suman uno al byte del cursor (para ajustar un valor poco a poco).
- **Texto** pasa el cursor a la columna de texto de la derecha y muestra el teclado completo: cada
  letra que pulses se escribe como un byte. Allí, la tecla **Hex** vuelve a la columna
  hexadecimal. Supr devuelve el byte del cursor a su valor original.

Seguridad:
- Los bytes cambiados salen en azul y el título lleva un `*` hasta que se guardan.
- El archivo no se toca hasta que eliges Guardar, y solo se sobrescriben los bytes cambiados:
  nunca se insertan ni se borran, así que el archivo conserva siempre su tamaño.
- Admite hasta 2048 bytes cambiados antes de guardar.

## 7. El visor de imágenes

Muestra imágenes **JPEG**, **PNG**, **GIF** y **BMP** de cualquier tamaño, ajustadas a la pantalla
(las pequeñas se amplían a un múltiplo exacto para que se vean nítidas).

| Botón | Acción |
|---|---|
| Izquierda / derecha | Imagen anterior / siguiente de la carpeta |
| A, B, GAME, TIME | Volver a la lista |

- JPEG: los normales (*baseline*, los de cámaras y móviles). Las fotos de más de 2560 px de ancho
  se decodifican a 1/8, que es rápido y sigue siendo más que la pantalla. **Los JPEG progresivos
  no están soportados** (sale un aviso).
- PNG: todos los tipos de color y profundidades, transparencia (sobre fondo negro) y entrelazados.
- GIF: las animaciones se reproducen con sus tiempos.
- BMP: 8, 16, 24 y 32 bits, como las capturas que hace el firmware con PAUSE + GAME.
- Carátulas de Retro-Go (`.img`, con JPEG dentro), para revisar las miniaturas de `/covers`.
- Con archivos grandes sale "Cargando..." un momento.

## 8. El reproductor de vídeo

Reproduce **archivos AVI con vídeo Motion-JPEG (como mucho 320x240) y sonido PCM o MP3**. Cada fotograma
lo decodifica el chip JPEG de la consola, y la imagen va sincronizada con el sonido. El nombre del
archivo sale arriba durante los 3 primeros segundos.

| Botón | Acción |
|---|---|
| A | Pausa / seguir (muestra el tiempo y una barra de progreso) |
| Izquierda / derecha | Atrás / adelante 10 segundos |
| Abajo / arriba | Atrás / adelante 1 minuto |
| B | Parar |
| PAUSE/SET | Menú de Retro-Go (volumen, brillo…); el vídeo se silencia mientras |
| PAUSE/SET + cruceta | Volumen / brillo, con el indicador sobre el vídeo |

Los vídeos del móvil o de internet (MP4, MKV, MOV, WebM… con H.264, H.265, VP9…) la consola no
los puede decodificar: al abrirlos sale un aviso. Conviértelos en el PC con
[ffmpeg](https://ffmpeg.org/) (Windows, macOS y Linux):

```
ffmpeg -i input.mp4 -vf "scale=320:240:force_original_aspect_ratio=decrease,scale=trunc(iw/2)*2:trunc(ih/2)*2,fps=15" -c:v mjpeg -q:v 5 -pix_fmt yuvj420p -c:a pcm_s16le -ar 22050 -ac 1 output.avi
```

La parte `-pix_fmt yuvj420p` es importante: el decodificador JPEG de la consola solo admite
fotogramas en color 4:2:0 (los MJPEG 4:2:2, 4:4:4 o en grises dan un aviso). Así sale un vídeo de
320x240 (o menos, manteniendo la forma) a 15 fotogramas por segundo con sonido
mono a 22 kHz. `-q:v` es la calidad (2 = la mejor y más grande, 10 = la más pequeña); `fps=15` se
puede subir si la tarjeta lee lo bastante rápido. Un clip de 4 minutos ocupa unos 30-40 MB. El
audio PCM a otras frecuencias se convierte al reproducir. El sonido MP3 también funciona y hace los
archivos más pequeños: usa `-c:a libmp3lame -b:a 96k -ar 22050 -ac 1` en lugar de la parte de
`pcm_s16le`. Otro sonido comprimido (AAC…) se reproduce sin sonido.

## 9. El reproductor de audio

Reproduce **MP3** (MPEG-1/2/2.5 capa 3, CBR o VBR; también capas 1 y 2, `.mp2`), **FLAC** (hasta 24
bits, cualquier nivel de compresión) **WAV** (PCM de 8, 16, 24 y 32 bits, coma flotante de 32
bits, A-law, µ-law, ADPCM IMA y de Microsoft) y **AIFF/AIFC**, mono o estéreo, a cualquier frecuencia. La consola tiene un solo altavoz, así que el estéreo se mezcla a mono.

En pantalla salen la onda que está sonando, el tiempo y la duración con una barra de progreso, y
el formato. Durante los 3 primeros segundos (y en pausa) una barra arriba muestra el título y el
artista de las etiquetas de la canción (ID3 o FLAC), o el nombre del archivo.

| Botón | Acción |
|---|---|
| A | Pausa / seguir |
| Izquierda / derecha | Atrás / adelante 10 segundos |
| Abajo / arriba | Atrás / adelante 1 minuto |
| TIME | Siguiente archivo de audio de la carpeta |
| GAME | Archivo de audio anterior |
| B | Parar (vuelve a la lista) |
| PAUSE/SET | Menú de Retro-Go; el sonido se pausa mientras |
| PAUSE/SET + cruceta | Volumen / brillo, con el indicador |

Cuando un archivo termina (o saltas más allá del final) suena el siguiente archivo de audio de la
carpeta, así que una carpeta funciona como un álbum. Otros formatos (OGG, Opus, M4A/AAC,
WMA…) dan un aviso, igual que los archivos sin audio (como una
descarga fallida que solo guardó las etiquetas y la carátula) y otros formatos renombrados a
`.mp3`; conviértelos con [ffmpeg](https://ffmpeg.org/):

```
ffmpeg -i entrada.m4a -c:a libmp3lame -q:a 2 salida.mp3
```

## 10. Consejos
- Para editar la configuración de un emulador o un `.cfg`, ábrelo con **GAME → Editar texto**.
- Para tocar una partida guardada, haz antes una copia (Copiar → Pegar en la misma carpeta crea
  `nombre (2).ext`) y edita el original en hexadecimal.
- Si un archivo de texto no se abre en el visor de texto por su extensión, usa
  **GAME → Ver como texto**.

---

## Firmware

Funciona con cualquier firmware Retro-Go SD 2.x (ABI v2, 908 bytes). El firmware oficial incluye
dos funciones más para él desde el commit
[d2b3868](https://github.com/sylverb/game-and-watch-retro-go-sd/commit/d2b3868c9c7980a0d48c28465234f12902528fdd)
("firmware ABI: expose rg_storage_rename and rg_storage_free_space", rama `external_cores`). Con un
firmware de ese commit en adelante hay estas mejoras:

| | Firmware anterior | Firmware con d2b3868 o posterior |
|---|---|---|
| Renombrar / mover | Copia y borra (lento con archivos grandes) | Instantáneo |
| Guardar en el editor de texto | Directamente sobre el archivo | Copia temporal + sustitución |
| Tarjeta SD (capacidad / libre) | No disponible | Sí |

El programa detecta solo qué firmware tiene. Lo hace leyendo esas dos funciones más allá de los
908 bytes del ABI, solo si el campo `size` del firmware las incluye (`src/fsops.c`).

## Compilar

Con el Arm GNU Toolchain 15.2.rel1 (el mismo que usa retro-go-sd):

    make GCC_PATH=/ruta/a/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin

O con Docker: `make docker`. Sale `build/FileManager.bin`.

Para probar la interfaz en español en el emulador sin cambiar el idioma del firmware:
`make CORE_C_DEFS="-DPROJECT_KIND_HOMEBREW=1 -DFM_TEST_ES"`.

`sdk/` es el SDK de cores/homebrews de Retro-Go SD (ABI v2 de 908 bytes, a propósito: así el
programa arranca también en firmwares sin las funciones nuevas).

## Código

- `src/main.c`: lista, menús, portapapeles, acciones, entrada.
- `src/fsops.c`: rutas y operaciones recursivas (copiar, mover, borrar, contar) con progreso.
- `src/osk.c`: teclado de nombres (pantalla completa).
- `src/kbd.c`: teclado acoplado abajo (texto, hexadecimal y columna de texto del editor hex).
- `src/editor.c`: editor de texto.
- `src/viewer.c`: visores de texto y hexadecimal (con el editor hexadecimal).
- `src/image.c`: visor de imágenes (JPEG, PNG, GIF, BMP) con un escalado por bandas.
- `src/video.c`: reproductor de vídeo AVI/MJPEG.
- `src/audio.c`: reproductor de audio MP3/FLAC/WAV; `src/flac.c`: un decodificador FLAC propio y
  pequeño.
- `src/third_party/`: [picojpeg](https://github.com/richgel999/picojpeg) (dominio público) y la
  parte de descompresión de [miniz](https://github.com/richgel999/miniz) (MIT), para JPEG y PNG;
  [minimp3](https://github.com/lieff/minimp3) (CC0) para MP3.

## Estado

Probado en el emulador gwemu, con el firmware `external_cores` con el cambio y sin él:

- navegación;
- visores de texto y hexadecimal;
- visor de imágenes: JPEG grande (a 1/8) y normal, JPEG en grises, aviso de JPEG progresivo, PNG
  (pequeño ampliado, RGBA, paleta, 16 bits, entrelazado), GIF animado, BMP, pasar de imagen con
  izquierda/derecha;
- reproductor de vídeo: reproducción con sonido, pausa, saltos adelante y atrás, final del
  archivo, vídeo sin sonido, sonido estéreo a 24 kHz (convertido) y los avisos de MPEG-4,
  tamaño excesivo y MP4;
- reproductor de audio: MP3 CBR/VBR/mono/22-48 kHz, MP2, WAV de 8/16/24 bits, flotante, A-law,
  µ-law, FLAC de 16/24 bits, mono, todos los modos estéreo y bloques de 8192 muestras (todos
  comparados muestra a muestra con ffmpeg), títulos ID3v1/v2, saltos, pausa,
  siguiente/anterior y final de archivo, el menú de Retro-Go y el indicador de volumen, y los
  avisos de OGG; WAV ADPCM IMA/MS y AIFF/AIFC (también comparados con ffmpeg); vídeo AVI con
  sonido MP3;
- copia de carpetas con subcarpetas: resultado idéntico byte a byte;
- mover (instantáneo y por copia + borrado);
- renombrar con el teclado;
- crear y borrar carpetas;
- selección múltiple;
- pegar en la misma carpeta;
- información, tarjeta SD y menú de PAUSE;
- editor hexadecimal: escribir cifras con el teclado, escribir texto en la columna de texto,
  guardar y descartar;
- editor de texto: escribir, borrar, mover, Mayús, guardar (CRLF y UTF-8 intactos), salir sin
  guardar y crear un archivo nuevo.

En la consola se ha probado la parte de archivos (navegar, copiar y sobrescribir, mover,
renombrar, borrar…), el visor de imágenes, el reproductor de vídeo (sonido PCM y MP3) y el de
audio. Los editores de texto y hexadecimal, de momento, solo en el emulador.

## Licencia

MIT (ver `LICENSE`). Las cabeceras de `sdk/include/` mantienen sus licencias originales; picojpeg
es de dominio público, miniz es MIT y minimp3 es CC0 (`src/third_party/`).
