#!/usr/bin/env python3
"""
schema_creator.py

Compare two Python "schema" files under:

    <project_root>/mediation/schemas/

and create a new combined schema file in the same directory.

- Script location:
      <project_root>/scripts/schema_creator.py

- Schema files:
      <project_root>/mediation/schemas/*.py

- Arguments `file1` and `file2` are usually just filenames
  (with or without `.py`), e.g. `basic_template` or
  `basic_template.py`. They are resolved to:

      mediation/schemas/<name>.py

- Each schema defines a top-level dict variable, e.g.:

      template_ifi_trade_schema = {
          "name":"template_ifi_trade_1",
          "type":3,
          "schema":{
              "description":"IFI - simple",
              "type":"object",
              "$schema":"https://json-schema.org/draft/2020-12/schema",
              "required":[ ... ],
              "properties":{
                  ...
              }
          }
      }

  or the simpler:

      ifi_extra_props = {
          "properties":{ ... }
      }

- The OUTER structure (metadata) from the *first* file is preserved.
  Only the `properties` dict is merged.

- Output style:
    * Double quotes for all strings
    * No space after colon ("name":"value")
    * Lists and dicts formatted like the example above

- Safety: the output file is NOT overwritten if it already exists
  (FileExistsError is raised).
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
    """Resolve a schema argument into an actual file path."""
    p = Path(arg)
    if p.is_absolute() or p.exists():
        return p
    if p.suffix != ".py":
        p = p.with_suffix(".py")
    return SCHEMAS_DIR / p.name


# ---------------------------------------------------------------------------
# Top-level dict extraction
# ---------------------------------------------------------------------------

def extract_first_dict_schema(path: Path) -> Tuple[str, Dict[str, Any]]:
    """
    Parse a Python file and return (variable_name, dict_value) for the
    first top-level assignment whose value is a dict literal.
    """
    with path.open("r", encoding="utf-8") as f:
        source = f.read()

    tree = ast.parse(source, filename=str(path))

    for node in tree.body:
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Dict):
            for t in node.targets:
                if isinstance(t, ast.Name):
                    name = t.id
                    value = ast.literal_eval(node.value)
                    if isinstance(value, dict):
                        return name, value

    raise ValueError(f"No top-level dict literal found in {path}")


def sanitize_var_name(name: str) -> str:
    """Ensure the variable name is a valid Python identifier."""
    return name if re.match(r"^[A-Za-z_]\w*$", name) else "combined_schema"


# ---------------------------------------------------------------------------
# Property merging
# ---------------------------------------------------------------------------

def describe_schema_type(val: Any) -> str:
    if isinstance(val, dict) and "type" in val:
        return f"{val['type']} (schema 'type')"
    return type(val).__name__


def compare_and_merge_schemas(
    schema1: Dict[str, Any],
    schema2: Dict[str, Any],
    label1: str,
    label2: str,
) -> Dict[str, Any]:
    """Merge two property dictionaries according to our rules."""
    combined: Dict[str, Any] = {}
    all_keys = sorted(set(schema1.keys()) | set(schema2.keys()))

    print(f"Comparing '{label1}' and '{label2}'")
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
                print(f"KEY '{key}': identical (taken from {label1})")
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
                        print(f"    {label1}: {t1!r}")
                        print(f"    {label2}: {t2!r}")

        print()

    print("-" * 80)
    print("Merge complete.")
    return combined


# ---------------------------------------------------------------------------
# Locating / reinserting properties
# ---------------------------------------------------------------------------

def locate_properties(schema: Dict[str, Any]) -> Tuple[Dict[str, Any], Tuple[str, ...]]:
    """
    Locate the 'properties' dict within a schema and return:

        (properties_dict, location_tuple)

    location_tuple indicates where the properties live:

        ("schema", "properties")  => schema["schema"]["properties"]
        ("properties",)          => schema["properties"]
        ()                       => treat whole dict as properties
    """
    if (
        isinstance(schema, dict)
        and "schema" in schema
        and isinstance(schema["schema"], dict)
        and "properties" in schema["schema"]
        and isinstance(schema["schema"]["properties"], dict)
    ):
        return schema["schema"]["properties"], ("schema", "properties")

    if "properties" in schema and isinstance(schema["properties"], dict):
        return schema["properties"], ("properties",)

    return schema, ()


def build_combined_schema_from_first(
    schema1: Dict[str, Any],
    combined_props: Dict[str, Any],
    location: Tuple[str, ...],
) -> Dict[str, Any]:
    """
    Reinsert combined_props into a copy of schema1 at the location where
    the original properties lived.
    """
    if not location:
        return combined_props

    new_schema = copy.deepcopy(schema1)

    if location == ("schema", "properties"):
        new_schema["schema"]["properties"] = combined_props
    elif location == ("properties",):
        new_schema["properties"] = combined_props
    else:
        new_schema = combined_props

    return new_schema


# ---------------------------------------------------------------------------
# Custom double-quote serializer matching your style
# ---------------------------------------------------------------------------

def _dq_string(s: str) -> str:
    """Return a double-quoted Python string literal."""
    escaped = (
        s.replace("\\", "\\\\")
         .replace('"', '\\"')
    )
    return f"\"{escaped}\""


def to_double_quote_python(obj: Any, indent: int = 0) -> str:
    """
    Serialize Python data to a string that is valid Python syntax BUT:
      - always uses double quotes
      - has no space after colon
      - formats dicts/lists like your example
    """
    IND = " " * 4
    pad = IND * indent

    # Strings
    if isinstance(obj, str):
        return _dq_string(obj)

    # Numbers, bool, None
    if obj is None or isinstance(obj, (int, float, bool)):
        return repr(obj)

    # Lists
    if isinstance(obj, list):
        if not obj:
            return "[]"
        inner_lines = [
            IND * (indent + 1) + to_double_quote_python(v, indent + 1)
            for v in obj
        ]
        return "[\n" + ",\n".join(inner_lines) + "\n" + pad + "]"

    # Dicts
    if isinstance(obj, dict):
        if not obj:
            return "{}"
        lines = []
        for k, v in obj.items():
            key_str = _dq_string(str(k))
            val_str = to_double_quote_python(v, indent + 1)
            # NOTE: no space after colon to match your style
            lines.append(IND * (indent + 1) + f"{key_str}:{val_str}")
        return "{\n" + ",\n".join(lines) + "\n" + pad + "}"

    # Fallback (shouldn't really happen)
    return _dq_string(str(obj))


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> None:
    parser = argparse.ArgumentParser(
        description="Merge schema 'properties' from two Python schema files."
    )
    parser.add_argument("file1", help="First (template) schema name/file")
    parser.add_argument("file2", help="Second schema name/file to merge in")
    parser.add_argument(
        "output_name",
        nargs="?",
        default="combined_schema",
        help="Output base name (default: combined_schema)",
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

    merged_props = compare_and_merge_schemas(props1, props2, label1, label2)

    combined_schema = build_combined_schema_from_first(schema1, merged_props, loc1)

    # Determine output file and variable name
    base = args.output_name
    if base.endswith(".py"):
        out_filename = Path(base).name
    else:
        out_filename = base + ".py"

    var_name = sanitize_var_name(Path(out_filename).stem)
    out_path = SCHEMAS_DIR / out_filename

    # Safety: don't overwrite
    if out_path.exists():
        msg = (
            f"ERROR: Output file already exists and will NOT be overwritten:\n"
            f"       {out_path}\n\n"
            f"Please choose a new output_name."
        )
        print(msg)
        raise FileExistsError(msg)

    body = to_double_quote_python(combined_schema, indent=0)

    SCHEMAS_DIR.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        f.write("# Auto-generated by schema_creator.py\n")
        f.write("# Combined schema from:\n")
        f.write(f"#   {path1}\n")
        f.write(f"#   {path2}\n\n")
        f.write(f"{var_name} = {body}\n")

    print(f"\nCombined schema written to: {out_path}")
    print(f"Schema variable name:       {var_name}")


if __name__ == "__main__":
    main()

