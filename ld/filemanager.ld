/*
 * File Manager linker script: the sdk's core_ram_emu.ld plus an overlay window.
 *
 * The viewers and players are not resident. Their object files are linked as three OVERLAY
 * sections that share one RAM window at the end of .bss (VMA __FM_OVL_START__), each with its
 * own LMA in a dummy store region so objcopy can extract them one by one. The packed
 * FileManager.bin carries them after the homebrew (tools/append_overlays.py) and src/ovl.c
 * reads the one a file needs into the window. Only the largest one counts toward RAM.
 *
 * Every object listed in an overlay must be excluded from the resident sections below.
 */

INCLUDE gnw_ram_emu.ld

/* RAM for the largest overlay (code + data + bss); the link fails if one outgrows it. */
__FM_OVL_LENGTH__ = 74K;

MEMORY
{
  RAM_EMU (xrw)   : ORIGIN = __RAM_EMU_START__, LENGTH = __RAM_EMU_LENGTH__
  OVL_STORE (r)   : ORIGIN = 0x60000000, LENGTH = 16M
}

PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  .core_entry __RAM_EMU_START__ :
  {
    KEEP(*(.core_entry))
    KEEP(*(.gw_core_bridge_probe))
    . = ALIGN(4);
    EXCLUDE_FILE(*viewer.o *editor.o *kbd.o *image.o *picojpeg.o *miniz_tinfl.o *video.o *audio.o *flac.o *ogg.o *tools.o *hash.o *extract.o *unzip.o *un7z.o *lzma.o *tinfl_tools.o) *(.text*)
    EXCLUDE_FILE(*viewer.o *editor.o *kbd.o *image.o *picojpeg.o *miniz_tinfl.o *video.o *audio.o *flac.o *ogg.o *tools.o *hash.o *extract.o *unzip.o *un7z.o *lzma.o *tinfl_tools.o) *(.rodata*)
    *(.glue_7)
    *(.glue_7t)
    . = ALIGN(4);
    __CORE_INIT_ARRAY_START__ = .;
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    __CORE_INIT_ARRAY_END__ = .;
  } >RAM_EMU :text

  .data :
  {
    EXCLUDE_FILE(*viewer.o *editor.o *kbd.o *image.o *picojpeg.o *miniz_tinfl.o *video.o *audio.o *flac.o *ogg.o *tools.o *hash.o *extract.o *unzip.o *un7z.o *lzma.o *tinfl_tools.o) *(.data*)
  } >RAM_EMU :data

  __CORE_CODE_END__ = .;

  .bss (NOLOAD) :
  {
    . = ALIGN(8);
    EXCLUDE_FILE(*viewer.o *editor.o *kbd.o *image.o *picojpeg.o *miniz_tinfl.o *video.o *audio.o *flac.o *ogg.o *tools.o *hash.o *extract.o *unzip.o *un7z.o *lzma.o *tinfl_tools.o) *(.bss*)
    EXCLUDE_FILE(*viewer.o *editor.o *kbd.o *image.o *picojpeg.o *miniz_tinfl.o *video.o *audio.o *flac.o *ogg.o *tools.o *hash.o *extract.o *unzip.o *un7z.o *lzma.o *tinfl_tools.o) *(COMMON)
    . = ALIGN(32);
    __FM_OVL_START__ = .;
    . = . + __FM_OVL_LENGTH__;
    __FM_OVL_END__ = .;
    . = ALIGN(8);
  } >RAM_EMU :data

  __CORE_BSS_END__ = .;

  ASSERT(__CORE_BSS_END__ <= __RAM_EMU_END__,
         "Error: File Manager code+data+bss (with the overlay window) overflows RAM_EMU")

  /* Overlays: same VMA (the window), consecutive LMAs in OVL_STORE. Within each, code and data
   * first, then bss: __fm_ovl_<n>_load is where the bytes stored in the file end. */
  OVERLAY __FM_OVL_START__ : NOCROSSREFS AT (0x60000000)
  {
    .fm_ovl_text
    {
      *viewer.o(.text* .rodata* .data*) *editor.o(.text* .rodata* .data*) *kbd.o(.text* .rodata* .data*)
      . = ALIGN(4);
      __fm_ovl_text_load = .;
      *viewer.o(.bss* COMMON) *editor.o(.bss* COMMON) *kbd.o(.bss* COMMON)
      . = ALIGN(8);
      __fm_ovl_text_end = .;
    }
    .fm_ovl_image
    {
      *image.o(.text* .rodata* .data*) *picojpeg.o(.text* .rodata* .data*) *miniz_tinfl.o(.text* .rodata* .data*)
      . = ALIGN(4);
      __fm_ovl_image_load = .;
      *image.o(.bss* COMMON) *picojpeg.o(.bss* COMMON) *miniz_tinfl.o(.bss* COMMON)
      . = ALIGN(8);
      __fm_ovl_image_end = .;
    }
    .fm_ovl_media
    {
      *video.o(.text* .rodata* .data*) *audio.o(.text* .rodata* .data*) *flac.o(.text* .rodata* .data*) *ogg.o(.text* .rodata* .data*)
      . = ALIGN(4);
      __fm_ovl_media_load = .;
      *video.o(.bss* COMMON) *audio.o(.bss* COMMON) *flac.o(.bss* COMMON) *ogg.o(.bss* COMMON)
      . = ALIGN(8);
      __fm_ovl_media_end = .;
    }
    .fm_ovl_tools
    {
      *tools.o(.text* .rodata* .data*) *hash.o(.text* .rodata* .data*) *extract.o(.text* .rodata* .data*)
      *unzip.o(.text* .rodata* .data*) *un7z.o(.text* .rodata* .data*) *lzma.o(.text* .rodata* .data*)
      *tinfl_tools.o(.text* .rodata* .data*)
      . = ALIGN(4);
      __fm_ovl_tools_load = .;
      *tools.o(.bss* COMMON) *hash.o(.bss* COMMON) *extract.o(.bss* COMMON) *unzip.o(.bss* COMMON)
      *un7z.o(.bss* COMMON) *lzma.o(.bss* COMMON) *tinfl_tools.o(.bss* COMMON)
      . = ALIGN(8);
      __fm_ovl_tools_end = .;
    }
  }

  ASSERT(__fm_ovl_text_end  <= __FM_OVL_END__, "Error: text overlay larger than the window")
  ASSERT(__fm_ovl_image_end <= __FM_OVL_END__, "Error: image overlay larger than the window")
  ASSERT(__fm_ovl_media_end <= __FM_OVL_END__, "Error: media overlay larger than the window")
  ASSERT(__fm_ovl_tools_end <= __FM_OVL_END__, "Error: tools overlay larger than the window")

  __exidx_start = .;
  __exidx_end = .;

  /DISCARD/ :
  {
    *(.fini_array*)
    *(.preinit_array*)
    *(.ARM.exidx*)
    *(.ARM.extab*)
    *(.comment)
    *(.ARM.attributes)
  }
}
