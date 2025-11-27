#!/usr/bin/env python3
"""
FULLY UPDATED VERSION — produces Python files with DOUBLE QUOTES ONLY.

This script:
- Reads schema Python files from mediation/schemas/
- Merges only the "properties" sections
- Preserves all metadata from the first file (name, type, schema fields, etc.)
- Writes a merged schema with:
    * double quotes instead of single quotes
    * preserved ordering
    * clean indentation
- NEVER overwrites an existing file (raises FileExistsError)
"""

import argparse
import ast
import copy
import re
from pathlib import Path
from typing import Any, Dict, Tuple


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SCHEMAS_DIR = PROJECT_ROOT / "mediation" / "schemas"


# ---------------------------------------------------------------------------
# Path resolution
# ---------------------------------------------------------------------------

def resolve_schema_path(arg: str) -> Path:
    p = Path(arg)
    if p.is_absolute() or p.exists():
        return p
    if p.suffix != ".py":
        p = p.with_suffix(".py")
    return SCHEMAS_DIR / p.name


# ---------------------------------------------------------------------------
# Extract top-level dict
# ---------------------------------------------------------------------------

def extract_first_dict_schema(path: Path) -> Tuple[str, Dict[str, Any]]:
    with path.open("r", encoding="utf-8") as f:
        source = f.read()

    tree = ast.parse(source, filename=str(path))

    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Dict):
            for t in node.targets:
                if isinstance(t, ast.Name):
                    name = t.id
                    value = ast.literal_eval(node.value)
                    return name, value

    raise ValueError(f"No dict literal found in {path}")


def sanitize_var_name(name: str) -> str:
    return name if re.match(r"^[A-Za-z_]\w*$", name) else "combined_schema"


# ---------------------------------------------------------------------------
# Property merging
# ---------------------------------------------------------------------------

def describe_schema_type(val: Any) -> str:
    if isinstance(val, dict) and "type" in val:
        return f"{val['type']} (schema 'type')"
    return type(val).__name__


def compare_and_merge_schemas(schema1, schema2, label1, label2):
    combined = {}
    keys = sorted(set(schema1) | set(schema2))

    print(f"Comparing '{label1}' and '{label2}'")
    print("-" * 80)

    for key in keys:
        if key not in schema2:
            combined[key] = schema1[key]
            print(f"KEY '{key}': only in {label1}")
        elif key not in schema1:
            combined[key] = schema2[key]
            print(f"KEY '{key}': only in {label2}")
        else:
            v1 = schema1[key]
            v2 = schema2[key]

            if v1 == v2:
                combined[key] = v1
                print(f"KEY '{key}': identical")
            else:
                combined[key] = v1  # prefer first file
                print(f"KEY '{key}': DIFFERENCE DETECTED")
                print(f"  {label1}: {describe_schema_type(v1)}")
                print(f"  {label2}: {describe_schema_type(v2)}")

                if isinstance(v1, dict) and isinstance(v2, dict):
                    t1 = v1.get("type")
                    t2 = v2.get("type")
                    if t1 != t2:
                        print("  Logical 'type' mismatch:")
                        print(f"    {label1}: {t1}")
                        print(f"    {label2}: {t2}")

    print("-" * 80)
    return combined


# ---------------------------------------------------------------------------
# Locate properties inside schema
# ---------------------------------------------------------------------------

def locate_properties(schema: Dict[str, Any]) -> Tuple[Dict[str, Any], Tuple[str, ...]]:
    if (
        isinstance(schema, dict)
        and "schema" in schema
        and isinstance(schema["schema"], dict)
        and "properties" in schema["schema"]
    ):
        return schema["schema"]["properties"], ("schema", "properties")

    if "properties" in schema and isinstance(schema["properties"], dict):
        return schema["properties"], ("properties",)

    return schema, ()


def build_combined_schema_from_first(schema1, props, location):
    new_schema = copy.deepcopy(schema1)

    if location == ("schema", "properties"):
        new_schema["schema"]["properties"] = props
    elif location == ("properties",):
        new_schema["properties"] = props
    else:
        new_schema = props

    return new_schema


# ---------------------------------------------------------------------------
# DOUBLE-QUOTE SERIALIZER (the magic you need)
# ---------------------------------------------------------------------------

def to_double_quote_python(obj, indent=0):
    """Serialize Python objects to Python syntax but ALWAYS using double quotes."""

    IND = " " * 4
    pad = IND * indent

    # Strings → always double quoted
    if isinstance(obj, str):
        escaped = obj.replace('"', r'\"')
        return f"\"{escaped}\""

    # None, bool, int, float → standard Python repr
    if obj is None or isinstance(obj, (int, float, bool)):
        return repr(obj)

    # Lists
    if isinstance(obj, list):
        if not obj:
            return "[]"
        inner = ",\n".join(
            pad + IND + to_double_quote_python(v, indent + 1) for v in obj
        )
        return "[\n" + inner + "\n" + pad + "]"

    # Dicts
    if isinstance(obj, dict):
        if not obj:
            return "{}"
        items = []
        for k, v in obj.items():
            key_str = f"\"{k}\""
            val_str = to_double_quote_python(v, indent + 1)
            items.append(pad + IND + f"{key_str}: {val_str}")
        return "{\n" + ",\n".join(items) + "\n" + pad + "}"

    raise TypeError(f"Unsupported type for serialization: {type(obj)}")


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("file1")
    parser.add_argument("file2")
    parser.add_argument("output_name", nargs="?", default="combined_schema")
    args = parser.parse_args(argv)

    path1 = resolve_schema_path(args.file1)
    path2 = resolve_schema_path(args.file2)

    name1, schema1 = extract_first_dict_schema(path1)
    name2, schema2 = extract_first_dict_schema(path2)

    props1, loc1 = locate_properties(schema1)
    props2, _ = locate_properties(schema2)

    merged_props = compare_and_merge_schemas(
        props1, props2, f"{path1.name}::{name1}", f"{path2.name}::{name2}"
    )

    combined_schema = build_combined_schema_from_first(schema1, merged_props, loc1)

    out_name = (
        Path(args.output_name).with_suffix(".py").name
        if not args.output_name.endswith(".py")
        else args.output_name
    )

    var_name = sanitize_var_name(Path(out_name).stem)
    out_path = SCHEMAS_DIR / out_name

    if out_path.exists():
        msg = f"\nERROR: Output file already exists:\n    {out_path}\n"
        print(msg)
        raise FileExistsError(msg)

    final_text = to_double_quote_python(combined_schema, 0)

    with out_path.open("w", encoding="utf-8") as f:
        f.write("# Auto-generated by schema_creator.py\n")
        f.write(f"# Combined from:\n#   {path1}\n#   {path2}\n\n")
        f.write(f"{var_name} = {final_text}\n")

    print(f"\nWrote combined schema to:\n  {out_path}")


if __name__ == "__main__":
    main()

