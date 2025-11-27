#!/usr/bin/env python3
"""
schema_creator.py

Compare two Python "schema" files living under:

    <project_root>/mediation/schemas/

and create a new combined schema file in the same directory.

Assumptions / design:

- This script itself is located at:
      <project_root>/scripts/schema_creator.py

- Schema files are stored in:
      <project_root>/mediation/schemas/

- CLI arguments `file1` and `file2` are usually just *filenames*
  (with or without the `.py` extension), e.g. `extra_template`
  or `extra_template.py`. These are automatically resolved into:

      <project_root>/mediation/schemas/<name>.py

- If you pass an explicit existing path (absolute or relative),
  that path is used as-is and *not* rewritten.

- Each schema file contains a top-level dict assignment like:

      ifi_extra_props = {
          "properties": {
              "trade_pg": {...},
              ...
          }
      }

- If the top-level dict contains a `"properties"` key whose value
  is a dict, the script compares and merges those `properties`
  dictionaries. The output schema has the same shape:

      combined_var = {"properties": { ...merged... }}

- For each property key:
    * Only in file1: taken from file1.
    * Only in file2: taken from file2.
    * In both and equal: take file1's value.
    * In both and different:
          - keep file1's value in the combined schema
          - print a detailed diff (including logical "type" field
            differences and structural subkey differences).

- The third optional CLI argument `output_name` controls the
  *variable name* and *filename* of the combined schema:

      python scripts/schema_creator.py extra_template mmkt_trades ifi_all_props

  writes:

      <project_root>/mediation/schemas/ifi_all_props.py

  containing:

      ifi_all_props = {"properties": {...}}

- SAFETY FEATURE:
    If the output file already exists, the script prints an error
    message and raises FileExistsError WITHOUT overwriting anything.
"""

import argparse
import ast
import os
import pprint
import re
from pathlib import Path
from typing import Any, Dict, Tuple


# ---------------------------------------------------------------------------
# Path resolution
# ---------------------------------------------------------------------------

PROJECT_ROOT = Path(__file__).resolve().parents[1]
SCHEMAS_DIR = PROJECT_ROOT / "mediation" / "schemas"


def resolve_schema_path(arg: str) -> Path:
    """
    Resolve a schema argument into an actual file path.

    Rules:
    - If `arg` is an absolute path or an existing relative path, use it as-is.
    - Otherwise, treat it as a schema name under SCHEMAS_DIR, adding `.py`
      if needed:

          arg = "extra_template"  ->  mediation/schemas/extra_template.py
          arg = "extra_template.py" -> mediation/schemas/extra_template.py
    """
    p = Path(arg)

    if p.is_absolute() or p.exists():
        return p

    if p.suffix != ".py":
        p = p.with_suffix(".py")

    return SCHEMAS_DIR / p.name


# ---------------------------------------------------------------------------
# Schema extraction and comparison
# ---------------------------------------------------------------------------

def extract_first_dict_schema(path: Path) -> Tuple[str, Dict[str, Any]]:
    """
    Parse a Python file and return (variable_name, dict_value) for the
    first top-level assignment whose value is a dict literal.

    Typical expected pattern inside schema files:

        ifi_extra_props = {
            "properties": {...}
        }
    """
    with path.open("r", encoding="utf-8") as f:
        source = f.read()

    try:
        tree = ast.parse(source, filename=str(path))
    except SyntaxError as e:
        raise ValueError(f"Could not parse {path}: {e}") from e

    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Dict):
            target_name = None
            for t in node.targets:
                if isinstance(t, ast.Name):
                    target_name = t.id
                    break
            if target_name is None:
                continue

            try:
                value = ast.literal_eval(node.value)
            except Exception as e:
                raise ValueError(
                    f"Could not evaluate dict literal for {target_name} in {path}: {e}"
                ) from e

            if not isinstance(value, dict):
                continue

            return target_name, value

    raise ValueError(f"No top-level dict literal found in {path}")


def sanitize_var_name(name: str) -> str:
    """Ensure the variable name is a valid Python identifier."""
    if re.match(r"^[A-Za-z_]\w*$", name):
        return name
    return "combined_schema"


def describe_schema_type(val: Any) -> str:
    """Human-readable type description."""
    if isinstance(val, dict) and "type" in val:
        return f"{val['type']} (schema 'type')"
    return type(val).__name__


def compare_and_merge_schemas(
    schema1: Dict[str, Any],
    schema2: Dict[str, Any],
    label1: str,
    label2: str,
) -> Dict[str, Any]:

    combined: Dict[str, Any] = {}

    keys1 = set(schema1.keys())
    keys2 = set(schema2.keys())
    all_keys = sorted(keys1 | keys2)

    print(f"Comparing schemas '{label1}' and '{label2}'")
    print("-" * 80)

    for key in all_keys:
        in1 = key in schema1
        in2 = key in schema2

        if in1 and not in2:
            combined[key] = schema1[key]
            print(f"KEY '{key}': only in {label1} (taken from {label1})")

        elif in2 and not in1:
            combined[key] = schema2[key]
            print(f"KEY '{key}': only in {label2} (taken from {label2})")

        else:
            v1 = schema1[key]
            v2 = schema2[key]

            if v1 == v2:
                combined[key] = v1
                print(f"KEY '{key}': identical in both (taken from {label1})")
            else:
                combined[key] = v1
                print(f"KEY '{key}': DIFFERENCE DETECTED")
                print(f"  {label1}: {describe_schema_type(v1)}")
                print(f"  {label2}: {describe_schema_type(v2)}")

                if isinstance(v1, dict) and isinstance(v2, dict):
                    type1 = v1.get("type")
                    type2 = v2.get("type")
                    if type1 != type2:
                        print("  Logical 'type' field:")
                        print(f"    {label1}: {type1!r}")
                        print(f"    {label2}: {type2!r}")

                    keys_v1 = set(v1.keys())
                    keys_v2 = set(v2.keys())
                    only_v1 = sorted(keys_v1 - keys_v2)
                    only_v2 = sorted(keys_v2 - keys_v1)

                    if only_v1:
                        print(f"  Only in {label1}: {only_v1}")
                    if only_v2:
                        print(f"  Only in {label2}: {only_v2}")

                    shared = sorted(keys_v1 & keys_v2)
                    diffs = [k for k in shared if v1[k] != v2[k]]
                    if diffs:
                        print("  Subkey mismatches:")
                        for d in diffs:
                            print(
                                f"    {d!r}: {label1} -> {v1[d]!r}, "
                                f"{label2} -> {v2[d]!r}"
                            )
                print()

    print("-" * 80)
    print("Merge complete.")
    return combined


def get_properties_view(schema: Dict[str, Any]) -> Tuple[Dict[str, Any], bool]:
    """Return (properties_dict, True) if schema contains 'properties'."""
    if (
        isinstance(schema, dict)
        and "properties" in schema
        and isinstance(schema["properties"], dict)
    ):
        return schema["properties"], True
    return schema, False


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> None:
    parser = argparse.ArgumentParser(
        description="Compare two schema files under mediation/schemas/."
    )
    parser.add_argument("file1")
    parser.add_argument("file2")
    parser.add_argument(
        "output_name",
        nargs="?",
        default="combined_schema",
        help="Output schema base name (default: combined_schema)",
    )

    args = parser.parse_args(argv)

    path1 = resolve_schema_path(args.file1)
    path2 = resolve_schema_path(args.file2)

    if not path1.exists():
        raise SystemExit(f"Schema file not found: {path1}")
    if not path2.exists():
        raise SystemExit(f"Schema file not found: {path2}")

    name1, schema1 = extract_first_dict_schema(path1)
    name2, schema2 = extract_first_dict_schema(path2)

    label1 = f"{path1.name}::{name1}"
    label2 = f"{path2.name}::{name2}"

    props1, has_props1 = get_properties_view(schema1)
    props2, has_props2 = get_properties_view(schema2)

    combined_props = compare_and_merge_schemas(props1, props2, label1, label2)

    combined_schema = (
        {"properties": combined_props}
        if has_props1 or has_props2
        else combined_props
    )

    base = args.output_name
    out_filename = (
        Path(base).with_suffix(".py").name
        if not base.endswith(".py")
        else Path(base).name
    )
    var_name = sanitize_var_name(Path(out_filename).stem)

    out_path = SCHEMAS_DIR / out_filename

    # ---------- SAFETY CHECK: DO NOT OVERWRITE ----------
    if out_path.exists():
        msg = (
            f"ERROR: Output file already exists and will NOT be overwritten:\n"
            f"       {out_path}\n\n"
            f"Please choose a new output_name."
        )
        print(msg)
        raise FileExistsError(msg)
    # -----------------------------------------------------

    schema_text = pprint.pformat(combined_schema, indent=4, sort_dicts=True)

    SCHEMAS_DIR.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        f.write("# Auto-generated by schema_creator.py\n")
        f.write("# Combined schema from:\n")
        f.write(f"#   {path1}\n")
        f.write(f"#   {path2}\n\n")
        f.write(f"{var_name} = {schema_text}\n")

    print(f"\nCombined schema written to: {out_path}")
    print(f"Schema variable name:       {var_name}")


if __name__ == "__main__":
    main()

