# Contexto para agentes: soporte PAL de BLACK

> Este archivo está solo en el fork `marianodparisi/Black-2006-Recomp`, rama `pal-support`. Es el handoff entre sesiones. **No lo incluyas en un PR al repo original** (`portingpete/Black-2006-Recomp`).

## Objetivo

- Correr **BLACK (Criterion, 2006)** nativo en PC a partir de la copia PAL europea de Xbox del usuario. Después, mejorar los gráficos sobre el renderer D3D11: sombras, texturas, AA, FSR.
- Es un proyecto personal que el usuario desarrolla **de forma independiente del repo original**: prueba en su PC con Windows. No abrir issues ni PRs upstream sin que lo pida.

## Desarrollo independiente

- `pal-support` es la rama principal del fork. El repo original (`upstream`) no hace falta; sus cambios se traen solo si el usuario lo pide: `git fetch upstream` y `git merge upstream/main`.
- Las dependencias del build también son forks del usuario, con una rama `black-pinned` que fija el commit:
  - `marianodparisi/xboxrecomp`: commit `766ecef`. Encima se aplica `patches/xboxrecomp.patch`.
  - `marianodparisi/dsp56300`: tag `v1.0.0`, commit `aab3649`.
  - `setup.ps1` y los workflows de CI clonan de ahí.
  - En el README y en `NOTICE` quedan las URLs originales, como crédito y para citar las licencias MIT.
- **Hooks nuevos en `recomp_manual.c` o `src/black_*`:** toda dirección literal de `.text` posterior al corte, de `.rdata`, de `.data` o de BSS va como `BLACK_VA(0x…)`, escrita en NTSC.
  - `test_manual_code_wraps_every_moving_address` (en `scripts/test_black_release.py`) falla si alguna queda sin envolver y dice archivo y línea.
  - Contra el `recomp_manual.c` original del autor detecta las 119 direcciones que hubo que envolver.
  - Los marcadores de hooks en `generate.py` con literales van por `RELEASE.text("…{0x…}…")`.
- **Después de agregar o cambiar direcciones**, correr `python scripts/verify_release.py --xbe game/default.xbe` (`--verbose` lista todas).
  - Verifica estáticamente cada `BLACK_VA` y cada placeholder de `generate.py` contra el XBE: código (dirección de retorno después de un `call`, o inicio de función) y datos (referencias de operandos de instrucciones, vecinos verificados, uso junto a una dirección verificada).
  - Sale con 1 si algo falla. Rechaza layouts incorrectos (probado con corrimientos de datos `0x10` y `0x40`, y de `.text` `0x20`).
  - Una dirección individual mal puesta puede pasar si cae sobre otra variable que se usa: es una red de seguridad, no una prueba.

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

- `pytest scripts tests/localization`: 44 pasan; 3 se saltean porque piden PowerShell.
- `generate.py` contra el XBE PAL real:
  - Termina sin errores: 13.805 funciones, 0 marcadores de falla.
  - Cada hook se aplicó la cantidad de veces esperada.
  - Todos los `sub_` que usa el C manual existen.
  - La dispatch mapea la dirección PAL al nombre canónico, por ejemplo `{ 0x0021C80Fu, sub_0021C7EF }`.
- **Launcher:** con mono, el launcher, la app de configuración y los ejecutables de tests compilan sin avisos. Los chequeos nuevos dieron 24/24, incluida la detección del XBE PAL real. La suite completa de WinForms no corre en macOS.
- **Cross-compile para Windows desde la Mac** (clang-cl 23, lld-link, xwin con el SDK 10.0.26100, Rust 1.98.1 con `x86_64-pc-windows-msvc`):
  - Compila y linkea `black_xbox_recomp.exe` completo, con el código generado del PAL: PE32+ x64, unos 60 MB, imports de D3D11, DXGI, XInput y Media Foundation.
  - Compilan todos los tests de C: `eeprom`, `scope_mask`, `viewmodel`, `weapon_*`, `movie_skip`, `black_audio_completion`, `kernel_*`, `apu_*` y los 7 de `ambient_occlusion`, cámara incluida.
  - **No se ejecutó el juego:** el `.exe` es para Windows.
- **Tests de C ejecutados en la Mac** como binarios x86_64 bajo Rosetta 2, con `tools/cross-macos/native-tests.sh <fuente> <build>`:
  - **9 suites pasan:** `scope_mask`, `viewmodel`, `weapon_trace`, `weapon_model_trace`, `weapon_runtime`, `weapon_model_math`, `kernel_timer_waitset`, `apu_timing` y `apu_output`. `scope_mask`, `viewmodel` y `weapon_trace` ejecutan código envuelto en `BLACK_VA`.
  - **`ambient_occlusion/camera`:** falla bajo Rosetta en el chequeo de MXCSR. Con el código original del autor falla igual, así que es un límite de Rosetta, no del port.
- **Tests de Windows bajo Wine 11.18** (build de Gcenx sin firmar, al que se le sacó la cuarentena con autorización del usuario), con `tools/cross-macos/wine-tests.sh <fuente> <build>`:
  - **Pasan 16 pruebas:** `eeprom-query`, las 6 de `movie_skip`, `black-audio-completion`, `kernel-dpc-queue`, `apu-pitch`, `apu-spatial` (2), `apu-physical-memory`, más los 3 controles `*-old-control`, que fallan como corresponde (`WILL_FAIL`).
  - **`eeprom-query` verificado con una mutación:** con un runtime que ignora `RECOMP_XBOX_LANGUAGE`, el test falla en los 3 chequeos de idioma.
  - **`ambient_occlusion`:** pasan `ao-menu`, `ao-post-order`, `ao-scene-retry` y `ao-focus-gap`. Fallan `ao-video`, `ao-warp` (no puede crear el shader D3D11) y las 2 de la cámara (MXCSR). Las 4 fallan igual con el código original del autor: son límites de Wine y Rosetta. `ao-launcher` se saltea (es PowerShell/.NET; ver la sección del launcher con mono).
- **Bug encontrado y corregido:** `tests/eeprom/test_eeprom.c` usaba `_putenv` sin incluir `<stdlib.h>`. Con MSVC compilaba solo con un aviso.
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
3. **Tests en Windows real:** correr `ctest` en la PC para confirmar lo que bajo Wine y Rosetta no se puede: `ao-video`, `ao-warp`, la cámara y `ao-launcher`.
4. **`sub_00240137`**, caso especial de `generate.py`: no aparece en la salida PAL. Probablemente venga del feedback de icall local del autor.
5. **Direcciones traducidas: verificadas estáticamente.** `verify_release.py` contra el XBE PAL da 73/73 ok, BSS incluido. Por ejemplo:
   - El vertex decode PAL (`0x205A0`) usa exactamente `0x295DA0`, `0x295DA4` y `0x295DAC`.
   - La fórmula del zoom en `0x1BD456` usa `[0x295C94]` (1.0) y `[0x295DD8]` (k) sobre `[cámara+0x8C]+0x2A4`.
   - Valores: `±70.0`, `0.01` y `1.0`/`1.3333`.
   - Las 15 direcciones de código caen después de un `call` o en un inicio de función.
   - Falta igual confirmarlo jugando.
6. **Mejoras gráficas**, después de que el juego corra.

## Herramientas útiles

- Leer el XBE: el header base está en `0x104`, el número y la dirección de las secciones en `0x11C`, y cada entrada de sección ocupa `0x38` bytes (flags, VA, tamaño virtual, offset raw, tamaño raw, puntero al nombre). Con eso se arma una función VA → offset en el archivo.
- Entry point retail: el valor en `0x128`, XOR `0xA8FC57AB`. Thunks del kernel: el valor en `0x158`, XOR `0x5B6D40B6`.
- Desensamblado: `capstone` 5.0.7 en modo x86-32 sobre esos offsets. El pipeline del repo usa la misma versión.
