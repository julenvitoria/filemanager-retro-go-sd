# File Manager — explorador de archivos para Game & Watch (Retro-Go SD)

Homebrew GWHB para el firmware Retro-Go SD 2.x (rama `external_cores`) que permite manejar la
tarjeta SD desde la propia consola.

## Qué hace

- **Navegar** por toda la SD, con las carpetas primero y en orden alfabético, y los tamaños.
- **Copiar, cortar (mover) y pegar** archivos y **carpetas completas**, con todo su contenido.
- **Renombrar** con un teclado en pantalla.
- **Borrar** archivos y carpetas completas, con confirmación (por defecto, "No").
- **Crear carpetas** y **archivos de texto nuevos**.
- **Editar texto** (`.txt`, `.cfg`, `.ini`…) con un teclado en pantalla abajo, al estilo de
  gw-dos-xtulator.
- **Selección múltiple**: TIME selecciona o quita la selección, y las acciones se aplican a todo lo seleccionado.
- **Ver archivos**:
  - de texto (`.txt`, `.cfg`, `.ini`, `.log`…);
  - en hexadecimal (cualquier archivo), **con editor**: cambia bytes sin alterar el tamaño del
    archivo y guarda solo cuando confirmas;
  - **imágenes BMP**, incluidas las capturas de pantalla del firmware (PAUSE + GAME).
- **Información** de un archivo o una carpeta: tamaño, fecha, y para carpetas cuántos archivos y
  subcarpetas tiene.
- **Tarjeta SD**: capacidad total, espacio usado y espacio libre.

Copiar, mover y borrar muestran una barra de progreso, y **B cancela** la operación (nunca deja un
archivo copiado a medias). Si al pegar ya existe algo con el mismo nombre, pregunta: sobrescribir,
omitir o cancelar. Si pegas una copia en la misma carpeta, crea `nombre (2).ext`.

Los textos salen en español o en inglés según el idioma del firmware.

## Instalación

Copia `FileManager.bin` a `/homebrews/` en la SD. Aparece en la pestaña Homebrew como "File Manager".

## Controles

| Botón | En la lista |
|---|---|
| Arriba / abajo | Moverse |
| Izquierda / derecha | Página anterior / siguiente |
| A | Abrir carpeta o ver archivo |
| B | Volver a la carpeta anterior |
| TIME | Seleccionar / quitar selección |
| GAME | Menú de acciones |
| PAUSE/SET | Menú de Retro-Go (brillo, volumen, salir, apagar) |

En el teclado en pantalla:

| Botón | Acción |
|---|---|
| Cruceta | Moverse por las teclas |
| A | Escribir la tecla |
| B | Borrar el último carácter |
| TIME | Cambiar entre minúsculas, mayúsculas y símbolos |
| GAME | Aceptar |

También tiene teclas de Espacio, Borrar, OK y Cancelar.

En el editor de texto (menú de acciones → "Editar texto", o A desde el visor de texto):

| Botón | Con el teclado | Sin el teclado (modo cursor) |
|---|---|---|
| Cruceta | Elegir tecla | Mover el cursor por el texto |
| A | Pulsar la tecla | Mostrar el teclado |
| B | Borrar el carácter anterior | Borrar el carácter anterior |
| TIME | Ocultar el teclado (más líneas a la vista) | Mostrar el teclado |
| GAME | Menú: guardar, guardar y salir, salir sin guardar | Igual |

El teclado tiene Mayús (una pulsación = la siguiente letra; dos = bloqueo), Tab, Intro, Supr,
flechas, Inicio/Fin y RePág/AvPág. Las líneas no se cortan: la vista se desplaza en horizontal.
Se conservan los finales de línea de Windows (CRLF) y los caracteres UTF-8 (á, ñ…), aunque la
letra de 8x8 los muestra sin tilde. Al guardar se escribe un archivo temporal y luego se
sustituye el original, así que un fallo no deja el archivo a medias (con el firmware sin
renombrado rápido se escribe directamente). Tamaño máximo para editar: 160 KB.

En el visor hexadecimal:

| Botón | Visor | Editando | Cambiando un byte |
|---|---|---|---|
| Cruceta | Mover / página | Mover el cursor | Arriba/abajo cambia la cifra, izquierda/derecha elige cifra |
| A | Empezar a editar | Cambiar el byte del cursor | Aceptar |
| B | Salir | Salir (pregunta si guardar, descartar o seguir) | Deshacer ese byte |
| GAME | — | Guardar | Aceptar |

Los bytes cambiados salen en azul y el título lleva un `*` hasta que se guardan. El archivo
no se toca hasta que eliges Guardar, y solo se sobrescriben los bytes cambiados (nunca se
insertan ni se borran).

## Firmware

Funciona con cualquier firmware Retro-Go SD 2.x (ABI v2, 908 bytes). Con un firmware que incluya
el commit "firmware ABI: expose rg_storage_rename and rg_storage_free_space" (rama
`external_cores` de julenvitoria/game-and-watch-retro-go-sd, 8080bb1) hay dos mejoras:

| | Firmware sin ese cambio | Firmware con ese cambio |
|---|---|---|
| Renombrar / mover | Copia y borra (lento con archivos grandes) | Instantáneo |
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
- `src/osk.c`: teclado en pantalla.
- `src/viewer.c`: visores de texto, hexadecimal (con editor) y BMP.
- `src/editor.c`: editor de texto con teclado en pantalla.

## Estado

Probado en el emulador gwemu, con el firmware `external_cores` con el cambio y sin él:

- navegación;
- visores de texto, hexadecimal y BMP de 16 y 24 bits;
- copia de carpetas con subcarpetas: resultado idéntico byte a byte;
- mover (instantáneo y por copia + borrado);
- renombrar con el teclado;
- crear y borrar carpetas;
- selección múltiple;
- pegar en la misma carpeta;
- información, tarjeta SD y menú de PAUSE;
- editor hexadecimal: cambiar, guardar y descartar;
- editor de texto: escribir, borrar, mover, Mayús, guardar (CRLF y UTF-8 intactos), salir sin
  guardar y crear un archivo nuevo.

También se ha probado en la consola.

## Licencia

MIT (ver `LICENSE`). Las cabeceras de `sdk/include/` mantienen sus licencias originales.
