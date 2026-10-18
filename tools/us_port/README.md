# US (GMSE01) support

`config/GMSE01/` was generated from the PAL (GMSP01) config, because the US disc has no linker map.
The build reproduces the US `main.dol` byte for byte: SHA1 `a6782903ef79d4196c8489ecb1b57decb5b3728f`, and `ninja` reports `build/GMSE01/mario.dol: OK`.

## Building

```
python configure.py --version GMSE01
ninja
```

Put the US disc image (or just `sys/main.dol`) in `orig/GMSE01/`.
The US build compiles as part of the PAL family: it defines both `VERSION_GMSP01` and `VERSION_GMSE01`.
Use `#ifdef VERSION_GMSE01` for code that differs in the US release.

## Per-version overrides

`config/GMSE01/nonmatching.txt` lists source files that link the original US object instead of compiled source, for the US build only.
This keeps the build byte-exact while the US-specific source isn't written yet.
The entries fall into three groups:

- Real US differences, such as 60 Hz video and render-mode code, regional menus, the boot code in `Application`, and SDK functions that differ between the US and PAL builds.
- Files whose US layout differs from PAL in ways not yet reflected in the source, such as JDrama struct offsets and inline functions shared between files.
- Placeholders caused by imperfect names in the tables.
  The 0x1178-byte `.bss` offset is fixed: the PAL translation had moved `dsptask.c`'s extra `AUDIO_YIELD_BUFFER` space (US: 0x2000 + `taskwork`, as in PAL) into `MapObjWater.cpp`, shifting every `.bss` split and symbol from `ModelGate.cpp` through `JPADraw.cpp` 0x1178 bytes too late.
  JSystem entries that were listed only because of that offset may now link from source, but they are JSystem and need a supervised check.

Removing an entry means getting that file's source to match the US build, usually by adding `VERSION_GMSE01` branches.
`python tools/us_compare.py GMSE01` lists the functions and data that differ.

## Regenerating the tables

`tools/us_port/build_config.sh WORKDIR` rebuilds `symbols.txt` and `splits.txt` from scratch.
WORKDIR has to contain these inputs:

- `us/main.dol`
- `us_sections.txt`
- `usana/symbols.txt` (from dtk's auto-analysis of the US DOL)
- `us_refs.json` (from `refs.py`)
- `align_*.json` (from `secalign.py`)

The pipeline runs these steps in order:

1. `translate2.py` translates PAL symbols to US addresses. It aligns functions by size sequence and aligns data sections by the spacing between symbols.
2. `refsplit.py` uses evidence from the US code to place file boundaries. US-only functions go to whichever file's vtables point at them, and a static initializer always ends its file. Data boundaries follow which file owns each named symbol and which file's code references each address.
3. `bss_refs.py` places each file's `.bss` start at the lowest address only that file's code uses.
4. `fixups.py` reassigns constructor-table slots from the real pointers, uses dtk file-start markers to fix boundaries, and trims oversized symbols.
5. `us_port_fix_labels.py` turns dtk's unsized labels into sized objects.
6. `drop_interior_markers.py` removes file-start labels that don't sit at a file's start.
7. `fill_gaps.py` covers data bytes that no symbol covers.

## Linker pitfalls found while doing this

mwldeppc hangs, with no error, when an object contains any of these:

- an untyped, unsized global label in a data section;
- a `...<sec>.0` file-start label that isn't at offset 0, or two of them in one object.

`scan_triggers.py` detects both.

It also drops code or data bytes that no sized symbol covers.
So never trim a function at a label inside it, for example `__DBVECTOR` inside `OSExceptionVector`.

For an object with several common symbols, mwldeppc reserves a block the size of all of them at the first one, then places the rest after it.
That reproduces the original layout as long as the PAL sizes are kept.
Don't fill or extend inside `common` ranges.
