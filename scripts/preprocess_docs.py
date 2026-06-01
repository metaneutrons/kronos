#!/usr/bin/env python3
"""
Preprocess pandoc-flavored Markdown for MkDocs Material.

Transforms:
  - Converts {#sec:label} anchors to MkDocs-compatible heading IDs
  - Resolves [@sec:label] cross-references into working hyperlinks
  - Strips pandoc YAML frontmatter blocks (--- ... ---)
  - Converts pandoc mermaid fences to standard ```mermaid blocks
  - Removes PNG image references (Mermaid renders natively)
  - Inlines .mmd file references as ```mermaid blocks

Usage: python3 scripts/preprocess_docs.py docs/ .site_src/docs/
"""

import re
import sys
from pathlib import Path


def build_anchor_map(src_dir: Path) -> dict:
    """
    Scan all .md files and build a map of {#sec:label} → (filename, heading_text).
    This allows cross-references to be resolved into working links.
    """
    anchor_map = {}
    for md_file in sorted(src_dir.glob("*.md")):
        for line in md_file.read_text().splitlines():
            match = re.search(r'^(#{1,4})\s+(.+?)\s*\{#(sec:[^}]+)\}', line)
            if match:
                heading_text = match.group(2).strip()
                label = match.group(3)
                # MkDocs generates slug from heading text
                slug = re.sub(r'[^\w\s-]', '', heading_text.lower())
                slug = re.sub(r'[\s]+', '-', slug).strip('-')
                anchor_map[label] = (md_file.name, heading_text, slug)
    return anchor_map


def preprocess(content: str, src_dir: Path, anchor_map: dict, current_file: str) -> str:
    """Transform a single Markdown file from pandoc to MkDocs flavor."""

    # Strip YAML frontmatter (only at file start)
    if content.startswith("---"):
        end = content.find("\n---", 3)
        if end != -1:
            content = content[end + 4:].lstrip("\n")

    # Convert pandoc mermaid fences to standard mermaid fences
    content = re.sub(r'```\{\.mermaid[^}]*\}', '```mermaid', content)

    # Remove heading anchor attributes but keep the heading text
    # ## Title {#sec:label} → ## Title
    content = re.sub(r'\s*\{#sec:[^}]+\}', '', content)

    # Resolve cross-references: [@sec:label] → [Section Title](file#slug)
    def resolve_ref(match):
        label = match.group(1)
        if label in anchor_map:
            filename, heading_text, slug = anchor_map[label]
            if filename == current_file:
                return f'[{heading_text}](#{slug})'
            else:
                return f'[{heading_text}]({filename}#{slug})'
        return f'*{label}*'  # Fallback: italicized label

    content = re.sub(r'\[@?(sec:[a-z0-9_-]+)\]', resolve_ref, content)

    # Resolve manual links: [text](#sec:label) → [text](file#slug)
    def resolve_manual_link(match):
        text = match.group(1)
        label = match.group(2)
        if label in anchor_map:
            filename, _, slug = anchor_map[label]
            if filename == current_file:
                return f'[{text}](#{slug})'
            else:
                return f'[{text}]({filename}#{slug})'
        return match.group(0)  # Leave unchanged if not found

    content = re.sub(r'\[([^\]]+)\]\(#(sec:[a-z0-9_-]+)\)', resolve_manual_link, content)

    # Remove PNG image references (Mermaid diagrams render natively in MkDocs)
    content = re.sub(r'!\[[^\]]*\]\([^)]+\.png\)(?:\{[^}]*\})?', '', content)

    # Remove remaining pandoc figure attributes: {#fig:label width=...}
    content = re.sub(r'\{#fig:[^}]+\}', '', content)

    # Inline .mmd diagram references
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
        return match.group(0)

    content = re.sub(
        r'!\[([^\]]*)\]\(([^)]+\.mmd)\)(?:\{[^}]*\})?',
        inline_mermaid,
        content
    )

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

    # Phase 1: Build anchor map from all source files
    anchor_map = build_anchor_map(src_dir)
    print(f"  Resolved {len(anchor_map)} cross-reference anchors")

    # Phase 2: Preprocess each file with resolved references
    for md_file in sorted(src_dir.glob("*.md")):
        content = md_file.read_text()
        processed = preprocess(content, src_dir, anchor_map, md_file.name)
        (dest_dir / md_file.name).write_text(processed)

    # Copy stylesheets/ directory
    css_src = src_dir / "stylesheets"
    css_dest = dest_dir / "stylesheets"
    if css_src.exists():
        css_dest.mkdir(parents=True, exist_ok=True)
        for f in css_src.iterdir():
            (css_dest / f.name).write_bytes(f.read_bytes())

    # Copy img/ directory (non-.mmd assets only)
    img_src = src_dir / "img"
    img_dest = dest_dir / "img"
    if img_src.exists():
        img_dest.mkdir(parents=True, exist_ok=True)
        for f in img_src.iterdir():
            if f.suffix != '.mmd':
                (img_dest / f.name).write_bytes(f.read_bytes())

    print(f"  Preprocessed {len(list(dest_dir.glob('*.md')))} files → {dest_dir}")


if __name__ == "__main__":
    main()
