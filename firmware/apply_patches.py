"""Pre-build hook: brengt de MeshCore-patches idempotent aan op de submodule.

Waarom een hook en geen handmatige `git apply`: de repo is de enige bron en de
gevendorde MeshCore-boom (firmware/vendor/MeshCore) moet in de repo ONaangeraakt
blijven staan op de gepinde commit. De patch(es) in firmware/patches/ worden pas
bij de build op de submodule aangebracht. Idempotent: draai je twee keer, dan
gebeurt er de tweede keer niets.

Waarom niet "in onze eigen boom oplossen met een build-flag": de patch vervangt
twee regels IN variants/heltec_v3/target.{h,cpp} (de hardgecodeerde
`EnvironmentSensorManager sensors`). Die bestanden compileren mee via MeshCore's
eigen build_src_filter; ze overschrijven zonder de upstream-bestanden uit te
sluiten kan niet, en uitsluiten+dupliceren zou net de drift terugbrengen die we
kwijt willen. De patch is drie regels en met terugval (zonder de macro's bouwt de
variant exact upstream), dus hem bij de build aanbrengen is de kleinste ingreep.
Zie firmware/patches/LEESMIJ.md.
"""

import os
import subprocess

Import("env")  # noqa: F821  (door PlatformIO/SCons ingebracht)

PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
SUBMODULE = os.path.join(PROJECT_DIR, "vendor", "MeshCore")
PATCH_DIR = os.path.join(PROJECT_DIR, "patches")

PATCHES = [
    "0001-sensor-manager-class-heltec-v3.patch",
]


def _git(args):
    return subprocess.run(
        ["git", "-C", SUBMODULE] + args,
        capture_output=True,
        text=True,
    )


def _apply(patch_path):
    # Al aangebracht? Dan lukt de omgekeerde controle.
    if _git(["apply", "--reverse", "--check", patch_path]).returncode == 0:
        print("  [patch] al aangebracht, overgeslagen: %s" % os.path.basename(patch_path))
        return
    # Nog niet aangebracht en past schoon? Aanbrengen.
    check = _git(["apply", "--check", patch_path])
    if check.returncode == 0:
        _git(["apply", patch_path])
        print("  [patch] aangebracht: %s" % os.path.basename(patch_path))
        return
    # Past niet: hard stoppen. Dan is de tag verschoven en moet een mens kijken;
    # een groene build op de verkeerde boom is erger dan een rode.
    raise SystemExit(
        "  [patch] KAN NIET AANBRENGEN: %s\n%s\n"
        "Is firmware/vendor/MeshCore nog op de gepinde tag?"
        % (os.path.basename(patch_path), check.stderr)
    )


if not os.path.isdir(os.path.join(SUBMODULE, ".git")) and not os.path.isfile(
    os.path.join(SUBMODULE, ".git")
):
    raise SystemExit(
        "  [patch] submodule ontbreekt: %s\n"
        "Draai eerst: git submodule update --init firmware/vendor/MeshCore" % SUBMODULE
    )

for name in PATCHES:
    _apply(os.path.join(PATCH_DIR, name))
