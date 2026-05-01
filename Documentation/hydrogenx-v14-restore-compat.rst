HydrogenX v14 restore-compatible add-on
======================================

This add-on is intended for the hydrogenx-v12-schedutil source branch after
HydrogenX has been committed directly into the kernel tree.

What it restores safely
-----------------------

* Adds ``kernel/sched/hydrogenx_sched_bridge.c`` so the weak SchedTune/UCLAMP
  hooks in ``hydrogenx_boost.c`` get strong best-effort implementations without
  patching the fragile vendor ``tune_plus.c`` file.
* Keeps the single stable build path under ``kernel/sched/Makefile``.  It does
  not re-add ``drivers/misc`` build glue, avoiding duplicate objects.
* Changes built-in zram default compressor from ``lzo`` to ``lz4`` when the
  stock line exists.
* Normalizes HydrogenX, F2FS, EROFS, zram, compression and BBR/FQ config blocks
  in the available pissarro defconfigs.
* Adds ``scripts/hydrogenx/verify-v14-restore.sh`` for on-device sanity checks.

What it intentionally does not restore
--------------------------------------

* It does not re-add fake LZ4K/LZ4KD/OPLUS aliases.  Those are not real codecs
  without codec and zram/crypto glue.
* It does not compile HydrogenX through ``drivers/misc``.  The boost controller
  is already built through ``kernel/sched``.
