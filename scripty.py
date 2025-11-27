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

- Each schema file contains a top-level dict assignment like either:

      ifi_mbs_trade_schema = {
          "name": "ifi_mbs_trade_6",
          "type": 3,
          "schema": {
              "description": "...",
              "type": "object",
              "$schema": "https://json-schema.org/draft/2020-12/schema",
              "required": [...],
              "properties": {
                  ...
              }
          }
      }

  or the simpler:

      ifi_extra_props = {
          "properties": { ... }
      }

- The script *preserves the outer structure from the first file*:
  everything except the `"properties"` section is copied as-is from
  the first schema file. Only the `"properties"` dictionary is merged.

- Property merge rules:
    * Only in file1: taken from file1.
    * Only in file2: taken from file2.
    * In both and equal: take file1's value.
    * In both and different:
          - keep file1's value in the combined schema
          - print a detailed diff (including logical "type" field
            differences and structural subkey differences).

- The third optional CLI argument `output_name` controls the
  *variable name* and *filename* of the combined schema:

      python scripts/schema_creator.py base_schema extra_template ifi_mbs_trades

  writes:

      <project_root>/mediation/schemas/ifi_mbs_trades.py

  containing something like:

      ifi_mbs_trades = {
          "name": "...",          # copied from first file
          "type": ...,
          "schema": {
              "description": "...",
              "type": "object",
              "$schema": "...",
              "required": [...],
              "properties": {
                  ... merged properties ...
              }
          }
      }

- SAFETY FEATURE:
    If the output file already exists, the script prints an error
    message and raises FileExistsError WITHOUT overwriting anything.
"""

import argparse
import ast
import copy
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

          arg = "extra_template"    -> mediation/schemas/extra_template.py
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

    Example patterns expected inside schema files:

        ifi_mbs_trade_schema = { ... }
        ifi_extra_props = { ... }
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
    """
    Compare two property dicts and produce a merged one.
    """
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
                combined[key] = v1  # prefer first file
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


def locate_properties(schema: Dict[str, Any]) -> Tuple[Dict[str, Any], Tuple[str, ...]]:
    """
    Locate the 'properties' dict within a schema and return:

        (properties_dict, location_tuple)

    where location_tuple is one of:
        ("schema", "properties")  -> schema["schema"]["properties"]
        ("properties",)           -> schema["properties"]
        ()                        -> no special nesting; treat whole
                                    dict as the properties dict
    """
    if (
        isinstance(schema, dict)
        and "schema" in schema
        and isinstance(schema["schema"], dict)
        and "properties" in schema["schema"]
        and isinstance(schema["schema"]["properties"], dict)
    ):
        return schema["schema"]["properties"], ("schema", "properties")

    if (
        isinstance(schema, dict)
        and "properties" in schema
        and isinstance(schema["properties"], dict)
    ):
        return schema["properties"], ("properties",)

    # Fallback: no special nesting, treat whole dict as properties
    return schema, ()


def build_combined_schema_from_first(
    schema1: Dict[str, Any],
    combined_props: Dict[str, Any],
    location: Tuple[str, ...],
) -> Dict[str, Any]:
    """
    Take the first schema as the "template" and insert combined_props
    back into the same place where its properties were located.

    This preserves metadata like:

        "name", "type", "schema": { "description", "$schema", "required", ... }
    """
    if not location:
        # No special nesting; just return the merged properties dict
        return combined_props

    combined_schema = copy.deepcopy(schema1)

    if location == ("schema", "properties"):
        combined_schema["schema"]["properties"] = combined_props
    elif location == ("properties",):
        combined_schema["properties"] = combined_props
    else:
        # Unexpected, but fall back to top-level replacement
        combined_schema = combined_props

    return combined_schema


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> None:
    parser = argparse.ArgumentParser(
        description="Compare two schema files under mediation/schemas/."
    )
    parser.add_argument("file1", help="First schema (base / template)")
    parser.add_argument("file2", help="Second schema to merge in")
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

    props1, loc1 = locate_properties(schema1)
    props2, _ = locate_properties(schema2)

    combined_props = compare_and_merge_schemas(props1, props2, label1, label2)

    # Build final combined schema by reusing outer structure from first file
    combined_schema = build_combined_schema_from_first(schema1, combined_props, loc1)

    # Determine output file and variable name
    base = args.output_name
    if base.endswith(".py"):
        out_filename = Path(base).name
    else:
        out_filename = base + ".py"

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

    # Pretty-print without re-sorting keys so structure/order from the first
    # file is mostly preserved.
    schema_text = pprint.pformat(
        combined_schema,
        indent=4,
        sort_dicts=False,
    )

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

