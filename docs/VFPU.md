# VFPU conventions used by `src/vfpu.c`

The Allegrex VFPU has 128 single-precision registers organised as eight
4×4 matrices (`M000`…`M700`). The naming of columns/rows and the operand
semantics of `vmmul`/`vtfm` are easy to get backwards, so the conventions
below were taken from the SDK's own known-good `libpspgum_vfpu.a` by
disassembling it (`psp-objdump -d`), not from memory.

| Fact | Evidence in libpspgum_vfpu |
|---|---|
| `lv.q Cm<i>0, 16*i(ptr)` loads memory vector *i* into **column** *i* of `Mm00`, so a column-major `Mat4` (`ScePspFMatrix4`) maps directly | `gumMultMatrix`, `gumLoadIdentity` |
| `vmmul.q Md, Ms, Mt` computes **D = S · T** | `gumMultMatrix(r, a, b)` loads a→M000, b→M100, then `vmmul.q M200, M000, M100` |
| `vtfm3/4 Cd, Ms, Cv` computes **d = Sᵀ · v** (dot with each column) | `gumFastInverse` computes `-Rᵀt` with `vtfm3.t C030, M200, C100` where M200 = R |
| so **M · v** uses the transposed view: `vtfm4.q Cd, E000, Cv` | follows from the row above |
| `vsin/vcos/vrot` take quarter-turns: angle × `VFPU_2_PI` | `gumRotateX`: `vcst.s S101, VFPU_2_PI; vmul.s …; vrot.q C010, S100, [0, c, s, 0]` |
| `vmmov.t Md, Esss` transposes the 3×3 part | `gumFastInverse` |

Other rules:

* The main thread is created with `PSP_THREAD_ATTR_VFPU`
  (`PSP_MAIN_THREAD_ATTR` in `main.c`); any future worker thread that uses
  `vfpu.c` must be created with that attribute too.
* `lv.q`/`sv.q` need 16-byte alignment. `Vec4`, `Mat4` and `Frustum` carry
  `aligned(16/64)`; GCC realigns such stack locals dynamically
  (`addiu v1,sp,15; ins v1,zero,0,4` in the generated code). Packed float3
  arrays go through `lv.s`/`sv.s`.
* GCC never allocates VFPU registers, so the batch loops load a matrix in
  one `asm` statement and reuse it across following statements.
* Angles are wrapped to [-π, π) before reaching `vsin`/`vcos`.

## Verification

* Host: `make host-test` runs the shared suite (`src/selftest.c`) against
  the scalar paths under ASan/UBSan.
* Device: the EBOOT runs the same suite at boot, comparing every VFPU
  routine against the scalar `ref_*` oracle on the real CPU, and prints
  `ALL SELF-TESTS PASSED` or the failing checks. Run it on a PSP-3000 (or in
  PPSSPP) before building Milestone 2 on top of these routines.
