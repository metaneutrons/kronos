#!/usr/bin/env python3
"""
Preprocess pandoc-flavored Markdown for MkDocs Material.

Transforms:
  - Removes {#sec:label} anchor attributes
  - Converts [@sec:label] cross-references to plain text
  - Strips pandoc YAML frontmatter blocks (--- ... ---)
  - Inlines .mmd file references as ```mermaid blocks
  - Passes everything else through unchanged

Usage: python3 scripts/preprocess_docs.py docs/ .site_src/docs/
"""

import re
import sys
from pathlib import Path


def preprocess(content: str, src_dir: Path) -> str:
    """Transform a single Markdown file from pandoc to MkDocs flavor."""

    # Strip YAML frontmatter (only at file start)
    if content.startswith("---"):
        end = content.find("\n---", 3)
        if end != -1:
            content = content[end + 4:].lstrip("\n")

    # Remove heading anchor attributes: ## Title {#sec:label}
    content = re.sub(r'\s*\{#sec:[^}]+\}', '', content)

    # Convert cross-references: [@sec:label] → (see documentation)
    # or [text](ref) style — just remove the bracket reference
    content = re.sub(r'\[?@sec:[a-z0-9_-]+\]?', '', content)
    # Also handle (see [@sec:...]) patterns
    content = re.sub(r'\(see\s*\)', '', content)

    # Inline .mmd diagram references:
    # ![caption](img/foo.mmd) → ```mermaid\n<content>\n```
    def inline_mermaid(match):
        caption = match.group(1)
        mmd_path = src_dir / match.group(2)
        if mmd_path.exists():
            mmd_content = mmd_path.read_text().strip()
            result = ""
            if caption:
                result += f"**{caption}**\n\n"
            result += f"```mermaid\n{mmd_content}\n```"
            return result
        return match.group(0)  # Leave unchanged if file not found

    content = re.sub(
        r'!\[([^\]]*)\]\(([^)]+\.mmd)\)(?:\{[^}]*\})?',
        inline_mermaid,
        content
    )

    # Convert pandoc mermaid fences to standard mermaid fences
    # ```{.mermaid caption="..." #fig:...}  →  ```mermaid
    content = re.sub(r'```\{\.mermaid[^}]*\}', '```mermaid', content)

    # Remove pandoc figure attributes: {#fig:label width=...}
    content = re.sub(r'\{#fig:[^}]+\}', '', content)

    # Remove PNG image references (Mermaid diagrams render natively in MkDocs)
    content = re.sub(r'!\[[^\]]*\]\([^)]+\.png\)(?:\{[^}]*\})?', '', content)

    # Clean up double blank lines
    content = re.sub(r'\n{3,}', '\n\n', content)

    return content


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <src_dir> <dest_dir>")
        sys.exit(1)

    src_dir = Path(sys.argv[1])
    dest_dir = Path(sys.argv[2])

    dest_dir.mkdir(parents=True, exist_ok=True)

    # Copy and preprocess .md files
    for md_file in sorted(src_dir.glob("*.md")):
        content = md_file.read_text()
        processed = preprocess(content, src_dir)
        (dest_dir / md_file.name).write_text(processed)

    # Copy img/ directory as-is (for any non-.mmd assets)
    img_src = src_dir / "img"
    img_dest = dest_dir / "img"
    if img_src.exists():
        img_dest.mkdir(parents=True, exist_ok=True)
        for f in img_src.iterdir():
            if f.suffix != '.mmd':  # Skip .mmd (inlined above)
                (img_dest / f.name).write_bytes(f.read_bytes())

    # Copy stylesheets/ directory
    css_src = src_dir / "stylesheets"
    css_dest = dest_dir / "stylesheets"
    if css_src.exists():
        css_dest.mkdir(parents=True, exist_ok=True)
        for f in css_src.iterdir():
            (css_dest / f.name).write_bytes(f.read_bytes())

    print(f"Preprocessed {len(list(dest_dir.glob('*.md')))} files → {dest_dir}")


if __name__ == "__main__":
    main()
