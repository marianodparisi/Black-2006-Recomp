# Contexto para agentes: soporte PAL de BLACK

> Este archivo está solo en el fork `marianodparisi/Black-2006-Recomp`, rama `pal-support`. Es el handoff entre sesiones. **No lo incluyas en un PR al repo original** (`portingpete/Black-2006-Recomp`).

## Objetivo

- Correr **BLACK (Criterion, 2006)** nativo en PC a partir de la copia PAL europea de Xbox del usuario. Después, mejorar los gráficos sobre el renderer D3D11: sombras, texturas, AA, FSR.
- Es un proyecto personal. Por ahora el usuario prefiere **probar en su PC con Windows antes de publicar nada upstream**: nada de issues ni PRs sin pedírselo.

## Reglas

- **Nunca commitear ni subir archivos del juego** (`.xbe`, `GlobData.bin`, `levels/`, `sound/`, `videos/`, `language/`…) ni código generado a partir de ellos.
  - `game/`, `src/recomp/`, `.work/` y `src/black_xmv_idct.inc` están en `.gitignore`. Verificalo igual antes de cada commit.
- Usar solo el dump del disco del usuario. Nada de dumps de terceros.
- El código del repo mezcla CRLF y LF dentro del mismo archivo. Al editar hay que preservar los fines de línea de cada línea, para no generar diffs gigantes.
- `patches/xboxrecomp.patch`: no regenerarlo entero con `git diff`, porque cambia el orden de los archivos y metería ruido en todo el diff.
  - Para modificarlo, editar el archivo en `third_party/xboxrecomp`.
  - Regenerar solo la sección de ese archivo: `git diff 766ecef -- <archivo>`. Las líneas de contexto en blanco del patch original van sin el espacio inicial.
  - Reemplazar esa sección dentro del patch.
  - Verificar que el patch aplique sobre un worktree limpio del commit fijado.

## Versiones del juego

| | NTSC (soportado upstream) | PAL (el del usuario) |
|---|---|---|
| SHA-256 `default.xbe` | `DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6` | `67F35121CCF622E7A293C402219596FE6456CA4C3D27E6F8139E449264DA30F0` |
| Title ID / entry point | `0x45410083` / `0x00025A3F` | igual |
| Tamaño de `.text` | `0x206FAC` | `0x206FDC` (**+0x30**) |
| Bancos de texto | `MainUS.bin` | `MainUK`, `MainFr`, `MainGe`, `MainIt`, `MainSp` |

Corrimiento de direcciones NTSC → PAL:

- `.text` hasta `0x115B40`: `+0x00`.
- `.text` desde `~0x1184D0`: `+0x30`.
- Todas las secciones después de `.text` (DSOUND, WMADEC, XMV, XGRPH, D3D, XPP, `.rdata`, `.data`, BSS…): `+0x20`. El fin de `.text` canónico es `0x217FC0`, y la traducción se aplica hasta `0x600000`.
- El punto exacto de inserción, entre `0x115B40` y `0x1184D0`, **sigue sin resolverse**. Para encontrarlo hace falta el XBE NTSC, que no tenemos. Ninguna dirección del repo cae en esa ventana.

## Diseño implementado

El NTSC es el **layout canónico**. Todo el código del repo sigue con direcciones NTSC, así queda una sola copia de cada hook.

1. **`scripts/black_release.py`**:
   - Detecta la versión por hash.
   - `to_guest` y `to_canonical` traducen en los dos sentidos.
   - `text("…{0x…}…")` traduce los literales de los marcadores de hooks.
   - `rename_symbols` renombra `sub_` y `loc_` del código generado a nombres canónicos.
   - `guest_symbols` pasa nombres canónicos a la versión local.
   - `header()` genera `src/recomp/black_release_gen.h`.
   - La ventana de inserción falla en lugar de adivinar. Las funciones dentro de ella quedan como `sub_XXXXXXXX_pal`.
2. **`scripts/generate.py`**:
   - `RELEASE` se elige en `main()`.
   - El escaneo de `recomp_manual.c` y las semillas de icall van con direcciones PAL, porque el recompilador saltea y envuelve funciones por dirección.
   - Después de generar, `canonicalize_generated()` renombra los símbolos a nombres canónicos.
   - Los literales del código (`MEM32(0x…)`, direcciones de retorno, tabla de dispatch) quedan con el valor PAL real.
   - La base de feedback de icall de PAL va a `local/icall_targets.pal.json`.
3. **C manual**:
   - 150 literales que se mueven están envueltos en `BLACK_VA(...)`, definido en `src/black_release.h`.
   - Es una expresión constante, así que funciona también en `case` e inicializadores estáticos.
   - Los literales de `.text` anteriores al corte no se tocaron, porque no se mueven.
   - Los tests de C que extraen código de `recomp_manual.c` definen `BLACK_VA(va) (va)`.
4. **Idioma (PAL)**, confirmado desensamblando el XBE PAL:
   - Al arrancar, `0x1F8154` llama a `0xC19A0`, que lee `XC_LANGUAGE`.
   - Guarda el índice en `[0x2D0D98]` (dirección PAL). Ese índice elige `Main%s.bin` en la tabla `0x288338` (PAL).
   - Mapeo: 1 → UK (US se fuerza a UK), 3 → Ge, 4 → Fr, 5 → Sp, 6 → It. Cualquier otro valor pasa a UK.
   - **Runtime** (dentro del patch, `kernel_xbox.c`): `XC_LANGUAGE` y el byte `0x90` del EEPROM salen de `RECOMP_XBOX_LANGUAGE` (1–9). Un valor inválido vuelve a inglés con un aviso.
   - **Launcher:**
     - `GameRelease` (en `scripts/game-language.cs`) detecta la versión por hash.
     - En PAL no instala bancos en `MainUS.bin` y pasa `RECOMP_XBOX_LANGUAGE` (es 5, fr 4, de 3, it 6, resto 1).
     - En PAL los idiomas aparecen como "official". pt-BR en PAL usa el texto del juego en inglés.
     - `RECOMP_LANGUAGE` sigue igual: traduce las indicaciones de teclado.
5. **`setup.ps1` y README**: aceptan y documentan las dos versiones.

## Verificado (en una Mac arm64, sin Windows)

- `pytest scripts tests/localization`: 42 pasan; 3 se saltean porque piden PowerShell.
- `generate.py` contra el XBE PAL real:
  - Termina sin errores: 13.805 funciones, 0 marcadores de falla.
  - Cada hook se aplicó la cantidad de veces esperada.
  - Todos los `sub_` que usa el C manual existen.
  - La dispatch mapea la dirección PAL al nombre canónico, por ejemplo `{ 0x0021C80Fu, sub_0021C7EF }`.
- **Launcher:** con mono, el launcher, la app de configuración y los ejecutables de tests compilan sin avisos. Los chequeos nuevos dieron 24/24, incluida la detección del XBE PAL real. La suite completa de WinForms no corre en macOS.
- **Cross-compile para Windows desde la Mac** (clang-cl 23, lld-link, xwin con el SDK 10.0.26100, Rust 1.98.1 con `x86_64-pc-windows-msvc`):
  - Compila y linkea `black_xbox_recomp.exe` completo, con el código generado del PAL: PE32+ x64, unos 60 MB, imports de D3D11, DXGI, XInput y Media Foundation.
  - Compilan todos los tests de C: `eeprom`, `scope_mask`, `viewmodel`, `weapon_*`, `movie_skip`, `black_audio_completion`, `kernel_*`, `apu_*` y los 7 de `ambient_occlusion`, cámara incluida.
  - **No se ejecutó nada:** el `.exe` es para Windows y la Mac no tiene Rosetta.
  - Ningún aviso viene de los cambios PAL. Los 1535 avisos son del código original o del generado; la mayoría son `-Wparentheses-equality`. Hay 6 `-Wsometimes-uninitialized` en `kelvin_pgraph.c` (renderer original, ramas `default` de varios `switch`), sin tocar.
  - Para clang hacen falta dos arreglos, ambos para cosas que MSVC tolera:
    - `qemu_shim.h` redefine `__builtin_ctz` y `__builtin_clz`.
    - `recomp_manual.c` redeclara `mcpx_apu_mmio_read` y `mcpx_apu_mmio_write` con `void *` en lugar de `MCPXAPUState *`.
    - Además, `main.c` usa `recomp_dispatch_init` sin declararla. Eso se resuelve con `-Wno-error=implicit-function-declaration` en el toolchain.
  - Los arreglos están en `tools/cross-macos/` (`xboxrecomp-clang-cl.patch` y `repo-clang-cl.diff`, que conserva sus CRLF) y se aplican **solo** en un worktree aparte, no en esta rama. Cómo armarlo: `tools/cross-macos/build.sh`.

## Pendiente

1. **Compilar y correr en Windows**: VS2022, CMake 3.20+, Python 3.10+, Rust con `x86_64-pc-windows-msvc`, CPU con AVX2.
   - Copiar el dump PAL en `game/`.
   - Ejecutar `.\scripts\setup.ps1` y después `.\'BLACK PC Launcher.exe'`.
   - `setup.ps1` debería decir "Found the PAL release of BLACK."
2. **Qué probar:**
   - Que arranque y que la primera misión funcione.
   - Español oficial: elegir "Español (official)" en el launcher y buscar `RECOMP_XBOX_LANGUAGE=5` en `reports\launcher-…`.
   - Velocidad normal: el runtime reporta NTSC a 60 Hz. `0x2F3A38` (canónico) debería valer `1/30`, con el check en `0x3D088889`.
   - Mouse (límites de pitch), zoom, HUD y textos de pickups. Usan muchos datos traducidos.
3. **Tests de C: compilan (cross-compile), pero no se corrieron.** En Windows hay que correrlos con `ctest`, en especial `tests/eeprom` (cubre `RECOMP_XBOX_LANGUAGE`) y los fixtures que extraen código de `recomp_manual.c`.
4. **`sub_00240137`**, caso especial de `generate.py`: no aparece en la salida PAL. Probablemente venga del feedback de icall local del autor.
5. **Datos de BSS** (`0x2F3BD4`, `0x2F3A38`…): se asumió `+0x20`. Solo se puede confirmar en ejecución.
6. **Mejoras gráficas**, después de que el juego corra.

## Herramientas útiles

- Leer el XBE: el header base está en `0x104`, el número y la dirección de las secciones en `0x11C`, y cada entrada de sección ocupa `0x38` bytes (flags, VA, tamaño virtual, offset raw, tamaño raw, puntero al nombre). Con eso se arma una función VA → offset en el archivo.
- Entry point retail: el valor en `0x128`, XOR `0xA8FC57AB`. Thunks del kernel: el valor en `0x158`, XOR `0x5B6D40B6`.
- Desensamblado: `capstone` 5.0.7 en modo x86-32 sobre esos offsets. El pipeline del repo usa la misma versión.
