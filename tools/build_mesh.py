"""Add zoom-level lens appearances to a vanilla scope mesh.

Works on a mesh as WolvenKit JSON (WolvenKit.CLI convert serialize). Every local
material that uses the scope lens shader (glass_scope.mt) gets one copy per zoom level,
and every vanilla appearance gets one copy per level that uses those lens copies.

Appearance names are "<skin>_szl<level>", e.g. "neon1_szl2". Level 0 is the
untouched vanilla appearance, so the vanilla look is always one step away.

For ArchiveXL resource patching, only the new appearances are kept, with all
materials: ArchiveXL looks material names up in the patch mesh itself.
"""
import copy
import json
import re

LENS_SHADER = "base\\materials\\glass_scope.mt"
SUFFIX = "_szl"

# Set on every zoom level: no edge-only warp, no colour fringing.
COMMON = {"SphereMaskDistortionRadius": 0.0, "AberrationStrenght": 0.0}


def cname(value):
    return {"$type": "CName", "$storage": "string", "$value": value}


def set_param(material, name, value):
    """Sets a float, a vector given as a list [x, y, z, w], or a colour given as
    {"rgb": [r, g, b]} (0-255)."""
    if isinstance(value, list):
        x, y, z, w = (list(value) + [0, 0, 0, 0])[:4]
        value = {"$type": "Vector4", "X": x, "Y": y, "Z": z, "W": w}
        kind = "Vector4"
    elif isinstance(value, dict) and "rgb" in value:
        r, g, b = value["rgb"]
        value = {"$type": "Color", "Alpha": 255, "Blue": b, "Green": g, "Red": r}
        kind = "Color"
    else:
        kind = "Float"
    for p in material["values"]:
        if name in p:
            p[name] = value
            return
    material["values"].append({"$type": kind, name: value})


def lens_materials(doc):
    """Names of the local materials that use the scope lens shader."""
    root = doc["Data"]["RootChunk"]
    buf = root.get("localMaterialBuffer") or {}
    mats = buf.get("materials") or []
    names = []
    for e in root["materialEntries"]:
        if not e.get("isLocalInstance") or e["index"] >= len(mats):
            continue
        base = mats[e["index"]].get("baseMaterial", {}).get("DepotPath", {}).get("$value", "")
        if base.lower() == LENS_SHADER:
            names.append(e["name"]["$value"])
    return names


def add_levels(doc, magnifications, overrides=None):
    """Adds the zoom levels to the mesh document in place and turns it into a patch.

    magnifications: one per level (level 1 first). Each becomes the lens material's
    DistortionStrenght, the glass_scope.mt parameter that enlarges the scene in the lens.
    overrides: extra lens material parameters for this scope, {"all": {...}, "1": {...}, ...}
    (per-level entries win over "all").

    Returns the lens material names that were found (empty: not a lens mesh, doc untouched).
    """
    overrides = overrides or {}
    levels = {i + 1: m for i, m in enumerate(magnifications)}
    lenses = lens_materials(doc)
    if not lenses:
        return []
    root = doc["Data"]["RootChunk"]
    buf = root["localMaterialBuffer"]
    files = buf["rawData"]["Data"]["Files"]
    by_name = {e["name"]["$value"]: e["index"] for e in root["materialEntries"]}

    next_index = len(buf["materials"])
    for lens in lenses:
        for level, strength in levels.items():
            mat = copy.deepcopy(buf["materials"][by_name[lens]])
            set_param(mat, "DistortionStrenght", strength)
            for k, v in COMMON.items():
                set_param(mat, k, v)
            for k, v in {**overrides.get("all", {}), **overrides.get(str(level), {})}.items():
                set_param(mat, k, v)
            raw =copy.deepcopy(files[by_name[lens]])
            raw["RootChunk"] = copy.deepcopy(mat)
            buf["materials"].append(mat)
            files.append(raw)
            buf["rawDataHeaders"].append({"$type": "meshLocalMaterialHeader", "offset": 0, "size": 0})
            root["materialEntries"].append({"$type": "CMeshMaterialEntry", "index": next_index,
                                            "isLocalInstance": 1, "name": cname(f"{lens}{SUFFIX}{level}")})
            next_index += 1

    # Handle IDs are unique across the whole file, not just the appearance list.
    handle = 1 + max(int(h) for h in re.findall(r'"HandleId": "(\d+)"', json.dumps(doc)))
    new_apps = []
    for app in root["appearances"]:
        skin = app["Data"]["name"]["$value"]
        for level in levels:
            new = copy.deepcopy(app)
            new["HandleId"] = str(handle)
            handle += 1
            new["Data"]["name"] = cname(f"{skin}{SUFFIX}{level}")
            new["Data"]["chunkMaterials"] = [
                cname(f"{c['$value']}{SUFFIX}{level}") if c["$value"] in lenses else c
                for c in app["Data"]["chunkMaterials"]
            ]
            new_apps.append(new)
    root["appearances"] = new_apps
    return lenses
